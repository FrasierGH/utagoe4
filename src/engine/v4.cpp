// Utagoe Rip 4 separation. Follows eval/proto.py (engine `v4`) step by step; where the
// prototype holds whole spectrograms, this streams STFT frames in several passes, so
// memory stays small for full-length songs.
#include "v4.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstdlib>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <tuple>

#include "dsp.hpp"

namespace utagoe {
namespace v4 {

namespace {

using Planar = std::vector<std::vector<double>>;
using CVec = std::vector<cplx>;
const double PI = 3.14159265358979323846;
const size_t N = 8192;          // STFT (as 3.0's ThVocalFFT)
const size_t HOPV = 1024;
const size_t NB = N / 2 + 1;    // one-sided bins

// fn(i, chunk) for i in [0, count), split into `chunks` contiguous runs on as many threads.
// Sums kept per chunk and added in chunk order give the same result on every machine.
const size_t CHUNKS = 16;
template <class Fn>
void parallel_for(size_t count, size_t chunks, Fn fn) {
    chunks = std::max<size_t>(1, std::min(chunks, count));
    if (chunks == 1) {
        for (size_t i = 0; i < count; i++) fn(i, (size_t)0);
        return;
    }
    std::vector<std::thread> th;
    for (size_t t = 0; t < chunks; t++)
        th.emplace_back([&, t] {
            for (size_t i = count * t / chunks; i < count * (t + 1) / chunks; i++) fn(i, t);
        });
    for (auto& x : th) x.join();
}

size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    double hi = v[m];
    if (v.size() % 2) return hi;
    double lo = *std::max_element(v.begin(), v.begin() + m);
    return 0.5 * (lo + hi);
}

std::vector<double> mono(const Planar& x) {
    std::vector<double> m(x[0].size());
    for (size_t i = 0; i < m.size(); i++) {
        double s = 0;
        for (const auto& ch : x) s += ch[i];
        m[i] = s / (double)x.size();
    }
    return m;
}

// x[start .. start + n) with zeros outside x
std::vector<double> segment(const std::vector<double>& x, long long start, size_t n) {
    std::vector<double> out(n, 0.0);
    long long lo = std::max(start, 0LL), hi = std::min(start + (long long)n, (long long)x.size());
    for (long long i = lo; i < hi; i++) out[(size_t)(i - start)] = x[(size_t)i];
    return out;
}

// One-sided spectra (n/2 + 1 bins) of two real signals with one complex FFT.
void rfft2(const std::vector<double>& x, const std::vector<double>& y, const FFT& f, CVec* X, CVec* Y) {
    size_t n = f.size();
    CVec z(n);
    for (size_t i = 0; i < n; i++) z[i] = cplx(i < x.size() ? x[i] : 0.0, i < y.size() ? y[i] : 0.0);
    f.forward(z.data());
    X->resize(n / 2 + 1);
    Y->resize(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; k++) {
        cplx zk = z[k], zc = std::conj(z[(n - k) % n]);
        (*X)[k] = (zk + zc) * 0.5;
        (*Y)[k] = (zk - zc) * cplx(0.0, -0.5);
    }
}

// numpy irfft: the inverse of a one-sided spectrum (imaginary parts of DC and Nyquist ignored).
std::vector<double> irfft(const CVec& h, const FFT& f) {
    size_t n = f.size();
    CVec z(n);
    z[0] = cplx(h[0].real(), 0.0);
    z[n / 2] = cplx(h[n / 2].real(), 0.0);
    for (size_t k = 1; k < n / 2; k++) {
        z[k] = h[k];
        z[n - k] = std::conj(h[k]);
    }
    f.inverse(z.data());
    std::vector<double> r(n);
    for (size_t i = 0; i < n; i++) r[i] = z[i].real() / (double)n;
    return r;
}

// ---------------------------------------------------------------- filters
// scipy.signal.butter(4, hz, fs=rate, output='sos') + sosfilt (zero initial state)
struct Biquad {
    double b0, b1, b2, a1, a2;
};

std::vector<Biquad> butter4_lowpass(double hz, double rate) {
    double wn = hz / (rate / 2);
    double warped = 4.0 * std::tan(PI * wn / 2.0);  // pre-warp, fs = 2 as scipy
    std::vector<std::complex<double>> poles;
    for (int m = -3; m <= 3; m += 2) poles.push_back(-std::exp(cplx(0, PI * m / 8.0)) * warped);
    double gain = std::pow(warped, 4);
    // bilinear transform, fs = 2
    std::vector<cplx> pd;
    cplx den = 1.0;
    for (const cplx& p : poles) {
        pd.push_back((4.0 + p) / (4.0 - p));
        den *= (4.0 - p);
    }
    double kd = gain / den.real();
    // conjugate pairs: (pd[0], pd[3]) and (pd[1], pd[2]); zeros at -1 (b = 1, 2, 1)
    std::vector<Biquad> s;
    for (int pair = 0; pair < 2; pair++) {
        cplx p = pd[pair];
        Biquad q{1.0, 2.0, 1.0, -2.0 * p.real(), std::norm(p)};
        if (pair == 0) q.b0 *= kd, q.b1 *= kd, q.b2 *= kd;
        s.push_back(q);
    }
    return s;
}

std::vector<double> sosfilt(const std::vector<Biquad>& sos, std::vector<double> x) {
    for (const Biquad& q : sos) {
        double z1 = 0, z2 = 0;
        for (double& v : x) {
            double y = q.b0 * v + z1;
            z1 = q.b1 * v - q.a1 * y + z2;
            z2 = q.b2 * v - q.a2 * y;
            v = y;
        }
    }
    return x;
}

// scipy.signal.butter(4, [100, 8000], 'band', fs=rate, output='sos')
std::vector<Biquad> butter4_bandpass(double lo, double hi, double rate) {
    double w1 = 4.0 * std::tan(PI * (lo / (rate / 2)) / 2.0), w2 = 4.0 * std::tan(PI * (hi / (rate / 2)) / 2.0);
    double bw = w2 - w1, wo = std::sqrt(w1 * w2);
    std::vector<cplx> p;
    for (int m = -3; m <= 3; m += 2) {
        cplx pl = -std::exp(cplx(0, PI * m / 8.0)) * (bw / 2);
        cplx root = std::sqrt(pl * pl - wo * wo);
        p.push_back(pl + root);
        p.push_back(pl - root);
    }
    double k = std::pow(bw, 4);
    cplx den = 1.0;
    std::vector<cplx> pd;
    for (const cplx& q : p) {
        pd.push_back((4.0 + q) / (4.0 - q));
        den *= (4.0 - q);
    }
    double kd = (k * std::pow(4.0, 4) / den).real();  // zeros at 0 map to +1, four more at -1
    std::vector<Biquad> s;
    for (const cplx& q : pd)
        if (q.imag() > 0) s.push_back({1.0, 0.0, -1.0, -2.0 * q.real(), std::norm(q)});
    s[0].b0 *= kd, s[0].b1 *= kd, s[0].b2 *= kd;
    return s;
}

std::vector<double> bandpass(const std::vector<double>& x, double rate) {
    static std::map<double, std::vector<Biquad>> cache;
    static std::mutex lock;
    std::vector<Biquad> sos;
    {
        std::lock_guard<std::mutex> hold(lock);
        auto it = cache.find(rate);
        if (it == cache.end()) it = cache.emplace(rate, butter4_bandpass(100.0, 8000.0, rate)).first;
        sos = it->second;
    }
    return sosfilt(sos, x);
}

std::vector<double> lowpass(const std::vector<double>& x, double rate) {
    static std::map<double, std::vector<Biquad>> cache;
    static std::mutex lock;
    std::vector<Biquad> sos;
    {
        std::lock_guard<std::mutex> hold(lock);
        auto it = cache.find(rate);
        if (it == cache.end()) it = cache.emplace(rate, butter4_lowpass(1000.0, rate)).first;
        sos = it->second;
    }
    return sosfilt(sos, x);
}

// trial subtraction a - g k with the least-squares g: (residual / original, g)
std::pair<double, double> trial(const std::vector<double>& a, const std::vector<double>& k) {
    double kk = 0, ak = 0, aa = 0;
    for (size_t i = 0; i < a.size(); i++) kk += k[i] * k[i], ak += a[i] * k[i], aa += a[i] * a[i];
    if (kk <= 0) return {INFINITY, 1.0};
    double g = ak / kk, r = 0;
    for (size_t i = 0; i < a.size(); i++) {
        double e = a[i] - g * k[i];
        r += e * e;
    }
    return {r / std::max(aa, 1e-20), g};
}

// scipy.ndimage.gaussian_filter1d(z, sigma, mode='nearest') (truncate = 4)
std::vector<double> gauss1d(const std::vector<double>& z, double sigma) {
    int radius = (int)(4.0 * sigma + 0.5);
    std::vector<double> w(2 * radius + 1);
    double s = 0;
    for (int i = -radius; i <= radius; i++) s += w[i + radius] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : w) v /= s;
    std::vector<double> out(z.size());
    long long n = (long long)z.size();
    for (long long i = 0; i < n; i++) {
        double acc = 0;
        for (int j = -radius; j <= radius; j++) acc += w[j + radius] * z[(size_t)std::clamp(i + j, 0LL, n - 1)];
        out[(size_t)i] = acc;
    }
    return out;
}

// np.interp(x, xp, fp) for increasing xp
double interp1(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
    if (x <= xp.front()) return fp.front();
    if (x >= xp.back()) return fp.back();
    size_t j = (size_t)(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin());
    double t = (x - xp[j - 1]) / (xp[j] - xp[j - 1]);
    return fp[j - 1] + t * (fp[j] - fp[j - 1]);
}

// ---------------------------------------------------------------- fractional reads
double bessel_i0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 200; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

struct Kernel {
    int half = 0, phases = 0;
    std::vector<double> tab;  // (phases + 1) x (2 half)
};

const Kernel& kernel(int half, double beta) {
    static std::map<std::pair<int, double>, Kernel> cache;  // entries never move or change
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    auto it = cache.find({half, beta});
    if (it != cache.end()) return it->second;
    Kernel k;
    k.half = half;
    k.phases = 1024;
    const double i0b = bessel_i0(beta);
    k.tab.resize((size_t)(k.phases + 1) * 2 * half);
    for (int p = 0; p <= k.phases; p++) {
        double fr = (double)p / k.phases;
        for (int j = 0; j < 2 * half; j++) {
            double u = (double)(j - half + 1) - fr;
            double sinc = u == 0 ? 1.0 : std::sin(PI * u) / (PI * u);
            double r = 1 - (u / half) * (u / half);
            k.tab[(size_t)p * 2 * half + j] = sinc * bessel_i0(beta * std::sqrt(std::max(r, 0.0))) / i0b;
        }
    }
    return cache.emplace(std::make_pair(half, beta), std::move(k)).first->second;
}

// x sampled at fractional positions pos (zero outside x); `chunks` threads
std::vector<double> frac_read(const std::vector<double>& x, const std::vector<double>& pos, int half,
                              double beta = 9.0, size_t chunks = 1) {
    const Kernel& K = kernel(half, beta);
    std::vector<double> out(pos.size(), 0.0);
    long long n = (long long)x.size();
    parallel_for(pos.size(), chunks, [&](size_t i, size_t) {
        double fl = std::floor(pos[i]);
        long long i0 = (long long)fl;
        long long q = (long long)std::nearbyint((pos[i] - fl) * K.phases);
        const double* t = &K.tab[(size_t)q * 2 * half];
        double acc = 0;
        for (int j = 0; j < 2 * half; j++) {
            long long idx = i0 + j - half + 1;
            if (idx >= 0 && idx < n) acc += x[(size_t)idx] * t[j];
        }
        out[i] = acc;
    });
    return out;
}

// ---------------------------------------------------------------- alignment
struct Track {
    std::vector<double> centres, lags, weights;
};

// local lag of b against a in windows (proto.track_lags): starts at the window nearest
// `seed` (default: the first) and tracks outwards; either polarity
Track track_lags(const std::vector<double>& a, const std::vector<double>& b, double rate, long long lag0,
                 double win_sec = 2.0, double hop_sec = 1.0, int search = 64, int search0 = 512,
                 size_t start = 0, size_t stop = SIZE_MAX, long long seed = -1) {
    size_t W = (size_t)(win_sec * rate), H = (size_t)(hop_sec * rate);
    if (stop > a.size()) stop = a.size();
    std::vector<double> win(W);
    for (size_t i = 0; i < W; i++) win[i] = 0.5 - 0.5 * std::cos(2.0 * PI * (double)i / (double)(W - 1));
    size_t n = next_pow2(2 * W);
    FFT f(n);
    std::vector<size_t> starts;
    long long last = std::max<long long>((long long)start + 1, (long long)stop - (long long)W + 1);
    for (long long s0 = (long long)start; s0 < last; s0 += (long long)H) starts.push_back((size_t)s0);
    size_t i0 = 0;
    if (seed >= 0) {
        double bestd = INFINITY;
        for (size_t i = 0; i < starts.size(); i++) {
            double d = std::fabs((double)starts[i] + (double)W / 2 - (double)seed);
            if (d < bestd) bestd = d, i0 = i;
        }
    }
    std::vector<size_t> order(starts.begin() + (long long)i0, starts.end());
    for (size_t i = i0; i-- > 0;) order.push_back(starts[i]);
    Track tr;
    long long centre = lag0, seed_lag = lag0;
    int rng = search0;
    for (size_t s : order) {
        if (i0 > 0 && s == starts[i0 - 1]) centre = seed_lag, rng = search0;  // turning back
        std::vector<double> sa = segment(a, (long long)s, W), sb = segment(b, (long long)s + centre, W);
        double ea = 0, eb = 0;
        for (size_t i = 0; i < W; i++) {
            sa[i] *= win[i];
            sb[i] *= win[i];
            ea += sa[i] * sa[i];
            eb += sb[i] * sb[i];
        }
        if (ea < 1e-10 || eb < 1e-10) continue;
        CVec SB, SA;
        rfft2(sb, sa, f, &SB, &SA);
        CVec X(n / 2 + 1), P(n / 2 + 1);
        for (size_t k = 0; k <= n / 2; k++) {
            X[k] = SB[k] * std::conj(SA[k]);
            P[k] = X[k] / (std::abs(X[k]) + 1e-12);
        }
        std::vector<double> r = irfft(P, f);
        long long d = 0;
        double best = -1, val = 0;
        for (int c = 0; c <= rng; c++)
            if (std::fabs(r[(size_t)c]) > best) best = std::fabs(r[(size_t)c]), val = r[(size_t)c], d = c;
        for (int c = -rng; c < 0; c++)
            if (std::fabs(r[n + c]) > best) best = std::fabs(r[n + c]), val = r[n + c], d = c;
        double pol = val >= 0 ? 1.0 : -1.0, peak = best;
        double num = 0, den = 0;
        for (size_t k = 0; k <= n / 2; k++) {
            double fhz = (double)k * rate / (double)n;
            if (!(fhz > 100 && fhz < 8000)) continue;
            double om = 2 * PI * fhz / rate;
            cplx y = pol * X[k] * std::exp(cplx(0, 2 * PI * fhz * (double)d / rate));
            double w = std::abs(y), ph = std::arg(y);
            num += w * om * ph;
            den += w * om * om;
        }
        double lag = (double)centre + (double)d - num / den;
        tr.centres.push_back((double)s + (double)W / 2);
        tr.lags.push_back(lag);
        tr.weights.push_back(peak);
        if (peak > 0.5 * median(tr.weights) || tr.weights.size() < 3) {
            centre = (long long)std::nearbyint(lag);
            rng = search;
            if (s == starts[i0]) seed_lag = centre;
        }
    }
    std::vector<size_t> idx(tr.centres.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return tr.centres[x] < tr.centres[y]; });
    Track sorted;
    for (size_t i : idx) {
        sorted.centres.push_back(tr.centres[i]);
        sorted.lags.push_back(tr.lags[i]);
        sorted.weights.push_back(tr.weights[i]);
    }
    return sorted;
}

// the lag at positions `at` (proto.fit_lag_curve); returns true if a straight line fitted
bool fit_lag_curve(const Track& tr, const std::vector<double>& at, double rate, std::vector<double>* out,
                   double sigma_sec = 1.5) {
    out->assign(at.size(), 0.0);
    size_t m = tr.lags.size();
    if (m == 0) return true;
    if (m < 3) {
        out->assign(at.size(), median(tr.lags));
        return true;
    }
    std::vector<char> keep(m);
    size_t nk = 0;
    for (size_t i = 0; i < m; i++) {
        std::vector<double> w5;
        for (long long j = (long long)i - 2; j <= (long long)i + 2; j++)
            w5.push_back(tr.lags[(size_t)std::clamp(j, 0LL, (long long)m - 1)]);
        keep[i] = std::fabs(tr.lags[i] - median(w5)) < 2.0;
    }
    // At an edge the running median sees mostly copies of the point itself and passes
    // anything, so the first and last two points are checked against the line through
    // their four inner neighbours instead; 8 samples, as the second pass (+-16) can
    // still correct anything closer
    if (m >= 6) {
        const size_t edge[4][2] = {{0, 1}, {1, 2}, {m - 1, m - 5}, {m - 2, m - 6}};
        for (const auto& e : edge) {
            size_t i = e[0], j0 = e[1];
            double mx = 0, my = 0, vx = 0, cxy = 0;
            for (size_t j = j0; j < j0 + 4; j++) mx += tr.centres[j] / 4, my += tr.lags[j] / 4;
            for (size_t j = j0; j < j0 + 4; j++) {
                vx += (tr.centres[j] - mx) * (tr.centres[j] - mx);
                cxy += (tr.centres[j] - mx) * (tr.lags[j] - my);
            }
            double slope = vx > 0 ? cxy / vx : 0.0;
            keep[i] = std::fabs(my + slope * (tr.centres[i] - mx) - tr.lags[i]) < 8.0;
        }
    }
    // windows whose correlation peak is weak (the instrumental nearly silent, say in an
    // a cappella passage) are not fitted: the curve is carried over them from the
    // reliable ones around (proto.MIN_WEIGHT)
    double wmin = 0.5 * median(tr.weights);
    for (size_t i = 0; i < m; i++) keep[i] = keep[i] && tr.weights[i] >= wmin, nk += keep[i];
    if (nk < 3) std::fill(keep.begin(), keep.end(), 1);
    std::vector<double> c, y, w;
    for (size_t i = 0; i < m; i++)
        if (keep[i]) c.push_back(tr.centres[i]), y.push_back(tr.lags[i]), w.push_back(tr.weights[i]);
    // weighted straight line (np.polyfit with w = sqrt(weights))
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < c.size(); i++) {
        sw += w[i], sx += w[i] * c[i], sy += w[i] * y[i];
        sxx += w[i] * c[i] * c[i], sxy += w[i] * c[i] * y[i];
    }
    double xm = sx / sw, ym = sy / sw;
    double vx = 0, cxy = 0;
    for (size_t i = 0; i < c.size(); i++) vx += w[i] * (c[i] - xm) * (c[i] - xm), cxy += w[i] * (c[i] - xm) * (y[i] - ym);
    double slope = vx > 0 ? cxy / vx : 0.0, icept = ym - slope * xm;
    std::vector<double> res;
    for (size_t i = 0; i < c.size(); i++) res.push_back(std::fabs(slope * c[i] + icept - y[i]));
    if (median(res) < 0.2) {
        for (size_t i = 0; i < at.size(); i++) (*out)[i] = slope * at[i] + icept;
        return true;
    }
    double s = sigma_sec * rate;
    for (size_t i = 0; i < at.size(); i++) {
        double t = at[i];
        double a0 = 0, a1 = 0, a2 = 0, b0 = 0, b1 = 0;
        for (size_t j = 0; j < c.size(); j++) {
            double z = (c[j] - t) / s;
            double k = w[j] * std::exp(-0.5 * z * z) + 1e-30;
            a0 += k, a1 += k * c[j], a2 += k * c[j] * c[j], b0 += k * y[j], b1 += k * c[j] * y[j];
        }
        double den = a0 * a2 - a1 * a1;
        double sl = std::fabs(den) > 1e-12 ? (a0 * b1 - a1 * b0) / den : 0.0;
        (*out)[i] = (b0 - sl * a1) / a0 + sl * t;
    }
    return false;
}

struct Cand {
    long long lag;
    int sign;
};

// candidates sorted by a static trial subtraction below 1 kHz (proto._rank); each gets
// the sign of its least-squares gain
std::vector<Cand> rank(const std::vector<double>& al, std::vector<double> bl, const std::vector<long long>& lags) {
    bl.resize(al.size(), 0.0);
    std::vector<std::pair<std::pair<double, long long>, int>> scored;
    std::vector<double> k(al.size());
    for (long long lag : lags) {
        for (size_t t = 0; t < al.size(); t++) {
            long long s = (long long)t + lag;
            k[t] = (s >= 0 && s < (long long)al.size()) ? bl[(size_t)s] : 0.0;
        }
        auto [res, g] = trial(al, k);
        scored.push_back({{res, lag}, g >= 0 ? 1 : -1});
    }
    std::sort(scored.begin(), scored.end());
    std::vector<Cand> out;
    for (auto& s : scored) out.push_back({s.first.second, s.second});
    return out;
}

// the two best GCC-PHAT lags (+-m) of up to 16 windows of 8 s spread over the song
std::vector<long long> window_peaks(const std::vector<double>& a, const std::vector<double>& b, double rate,
                                   long long m, long long sep) {
    size_t W = (size_t)(8.0 * rate), H = (size_t)(4.0 * rate);
    std::vector<long long> out;
    if (a.size() < W) return out;
    size_t count = std::min<size_t>(16, (a.size() - W) / H + 1);
    size_t n = next_pow2(W + W + 2 * (size_t)m);
    FFT f(n);
    std::vector<std::vector<long long>> found(count);
    parallel_for(count, count, [&](size_t i, size_t) {
        size_t w0 = count > 1 ? (size_t)((double)i * (double)(a.size() - W) / (double)(count - 1)) : 0;
        std::vector<double> sa(a.begin() + (long long)w0, a.begin() + (long long)(w0 + W));
        std::vector<double> sb = segment(b, (long long)w0 - m, W + 2 * (size_t)m);
        CVec B, A;
        rfft2(sb, sa, f, &B, &A);
        for (size_t k = 0; k < B.size(); k++) {
            cplx x = B[k] * std::conj(A[k]);
            B[k] = x / (std::abs(x) + 1e-12);
        }
        std::vector<double> r = irfft(B, f);
        long long first = 0, second = LLONG_MIN;
        double v1 = -1, v2 = -1;
        for (long long j = 0; j <= 2 * m; j++)
            if (std::fabs(r[(size_t)j]) > v1) v1 = std::fabs(r[(size_t)j]), first = j;
        for (long long j = 0; j <= 2 * m; j++)
            if (std::llabs(j - first) > sep && std::fabs(r[(size_t)j]) > v2) v2 = std::fabs(r[(size_t)j]), second = j;
        found[i].push_back(first - m);
        if (second != LLONG_MIN) found[i].push_back(second - m);
    });
    for (const auto& v : found) out.insert(out.end(), v.begin(), v.end());
    return out;
}

// global lag candidates, best trial subtraction first (proto.find_lag)
std::vector<Cand> find_lags(const std::vector<double>& a, const std::vector<double>& b, double rate,
                            double max_sec = 10.0, size_t candidates = 6, long long sep = 256, double excerpt_sec = 60.0) {
    long long m = (long long)(max_sec * rate);
    size_t E = (size_t)(excerpt_sec * rate);
    std::vector<long long> lags;
    std::vector<double> vals;
    std::vector<double> ae, be;
    if (a.size() <= E) {
        ae = a;
        be = b;
    } else {
        size_t a0 = a.size() / 2 - E / 2;
        ae.assign(a.begin() + (long long)a0, a.begin() + (long long)(a0 + E));
        be = segment(b, (long long)a0 - m, E + 2 * (size_t)m);
    }
    size_t n = next_pow2(ae.size() + be.size());
    {
        FFT f(n);
        CVec B, A;
        rfft2(be, ae, f, &B, &A);
        for (size_t k = 0; k < B.size(); k++) {
            cplx x = B[k] * std::conj(A[k]);
            B[k] = x / (std::abs(x) + 1e-12);
        }
        std::vector<double> r = irfft(B, f);
        if (a.size() <= E) {
            // a short file covers fewer lags (n >= |a| + |b|, so these do not wrap)
            long long mp = std::min(m, (long long)b.size()), mn = std::min(m, (long long)a.size());
            for (long long d = 0; d < mp; d++) lags.push_back(d), vals.push_back(r[(size_t)d]);
            for (long long d = -mn; d < 0; d++) lags.push_back(d), vals.push_back(r[(size_t)((long long)n + d)]);
        } else {
            for (long long d = 0; d <= 2 * m; d++) lags.push_back(d - m), vals.push_back(r[(size_t)d]);
        }
    }
    std::vector<size_t> order(vals.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t i, size_t j) { return std::fabs(vals[i]) > std::fabs(vals[j]); });
    std::vector<long long> cands;
    for (size_t j : order) {
        bool ok = true;
        for (long long c : cands) ok = ok && std::llabs(lags[j] - c) > sep;
        if (ok) {
            cands.push_back(lags[j]);
            if (cands.size() == candidates) break;
        }
    }
    return rank(lowpass(a, rate), lowpass(b, rate), cands);
}

// lag and polarity (proto.pick_tracked): whole-file candidates plus the best window
// candidates, each tracked through an excerpt and scored by a 100 Hz - 8 kHz trial
// subtraction along its own curve. Returns the winner's lag at `pos`, the excerpt's middle.
struct Pick {
    long long lag;
    int sign;
    size_t pos;
};

Pick pick_tracked(const std::vector<double>& a, const std::vector<double>& b, double rate, size_t top_windows = 4,
                  double excerpt_sec = 60.0, long long same = 512, double max_sec = 10.0) {
    size_t n = a.size();
    long long m = (long long)(max_sec * rate);
    std::vector<Cand> cands;
    auto add = [&](const Cand& c) {
        for (const Cand& k : cands)
            if (std::llabs(c.lag - k.lag) <= same) return false;
        cands.push_back(c);
        return true;
    };
    for (const Cand& c : find_lags(a, b, rate)) add(c);
    std::vector<Cand> extra = rank(lowpass(a, rate), lowpass(b, rate), window_peaks(a, b, rate, m, 256));
    size_t added = 0;
    for (const Cand& c : extra) {
        if (added == top_windows) break;
        if (add(c)) added++;
    }
    size_t E = std::min(n, (size_t)(excerpt_sec * rate)), e0 = (n - E) / 2, mid = e0 + E / 2;
    std::vector<double> ab = bandpass(std::vector<double>(a.begin() + (long long)e0, a.begin() + (long long)(e0 + E)), rate);
    std::vector<double> sub;
    for (size_t t = e0; t < e0 + E; t += 256) sub.push_back((double)t);
    std::vector<double> res(cands.size()), gain(cands.size()), lag_mid(cands.size());
    parallel_for(cands.size(), cands.size(), [&](size_t j, size_t) {
        Track tr = track_lags(a, b, rate, cands[j].lag, 2.0, 1.0, 64, 512, e0, e0 + E, (long long)mid);
        std::vector<double> curve;
        fit_lag_curve(tr, sub, rate, &curve);
        std::vector<double> pos(E);
        for (size_t i = 0; i < E; i++) pos[i] = (double)(e0 + i) + interp1((double)(e0 + i), sub, curve);
        std::vector<double> k = bandpass(frac_read(b, pos, 16), rate);
        std::tie(res[j], gain[j]) = trial(ab, k);
        lag_mid[j] = interp1((double)mid, sub, curve);
    });
    Pick best{cands.front().lag, 1, mid};
    double best_res = INFINITY;
    for (size_t j = 0; j < cands.size(); j++)
        if (res[j] < best_res) {
            best_res = res[j];
            best = {(long long)std::nearbyint(lag_mid[j]), gain[j] >= 0 ? 1 : -1, mid};
        }
    return best;
}

// ---------------------------------------------------------------- STFT frames
// Framing as scipy.signal.stft (Hann, boundary='zeros', padded=True): frame k is
// centred on sample k * HOP; spectra are scaled by 1 / sum(window).
struct Stft {
    size_t n = 0, frames = 0, C = 0;
    std::vector<double> win;
    double wsum = 0;
    FFT fft{N};
    explicit Stft(size_t len, size_t channels) : n(len), C(channels), win(N) {
        for (size_t j = 0; j < N; j++) win[j] = 0.5 - 0.5 * std::cos(2.0 * PI * (double)j / (double)N);
        wsum = std::accumulate(win.begin(), win.end(), 0.0);
        size_t nadd = (HOPV - len % HOPV) % HOPV;
        frames = (len + nadd) / HOPV + 1;
    }
    // spectra (C x NB) of x's frame k, read at lag (fraction as a linear phase)
    void frame(const Planar& x, size_t k, double lag, std::vector<CVec>* out) const {
        double li = std::floor(lag), fr = lag - li;
        long long start = (long long)(k * HOPV) - (long long)(N / 2) + (long long)li;
        long long len = (long long)x[0].size();
        CVec z(N);
        for (size_t j = 0; j < N; j++) {
            long long t = start + (long long)j;
            bool in = t >= 0 && t < len;
            double re = in ? x[0][(size_t)t] : 0.0;
            double im = (in && C > 1) ? x[1][(size_t)t] : 0.0;
            z[j] = cplx(re, im) * win[j];
        }
        fft.forward(z.data());
        out->assign(C, CVec(NB));
        for (size_t b = 0; b < NB; b++) {
            cplx zk = z[b], zc = std::conj(z[(N - b) % N]);
            cplx ph = fr != 0.0 ? std::exp(cplx(0, 2 * PI * (double)b / (double)N * fr)) : cplx(1.0, 0.0);
            if (C == 1) {
                (*out)[0][b] = zk / wsum * ph;
            } else {
                (*out)[0][b] = (zk + zc) * 0.5 / wsum * ph;
                (*out)[1][b] = (zk - zc) * cplx(0.0, -0.5) / wsum * ph;
            }
        }
    }
};

// sum over +-octave/2 around every bin (at least +-2 bins)
CVec smooth_bins(const CVec& z, double octave) {
    size_t nb = z.size();
    CVec c(nb + 1, 0.0);
    for (size_t k = 0; k < nb; k++) c[k + 1] = c[k] + z[k];
    CVec out(nb);
    for (size_t k = 0; k < nb; k++) {
        double kk = (double)k;
        long long lo = (long long)std::max(0.0, std::min(kk - 2, std::floor(kk * std::pow(2.0, -octave / 2))));
        long long hi = (long long)std::min((double)nb - 1, std::max(kk + 2, std::ceil(kk * std::pow(2.0, octave / 2))));
        out[k] = c[(size_t)hi + 1] - c[(size_t)lo];
    }
    return out;
}

double phase_diff(const cplx& a, const cplx& b) {
    double d = std::fabs(std::arg(a) - std::arg(b));
    return d > PI ? 2 * PI - d : d;
}

}  // namespace

Planar separate(const Planar& mix_in, const Planar& kar_in, int rate_i, const Options& opt, Report* report,
                const std::function<bool(int)>& progress) {
    auto step = [&](int pct) { return !progress || progress(pct); };
    const double rate = (double)rate_i;
    const size_t n = mix_in[0].size(), C = mix_in.size();
    Report rep;

    // --- global lag and polarity, from the best tracked candidate
    std::vector<double> am = mono(mix_in), bm = mono(kar_in);
    Pick c0 = pick_tracked(am, bm, rate);
    rep.lag = (long)c0.lag;
    rep.sign = c0.sign;
    Planar kar = kar_in;
    for (auto& ch : kar)
        for (double& v : ch) v *= c0.sign;
    for (double& v : bm) v *= c0.sign;
    if (!step(15)) return {};

    // --- frame lags: two coarse passes, then per-frame refinement unless a line fits
    Stft st(n, C);
    const size_t F = st.frames;
    std::vector<double> centres(F);
    for (size_t k = 0; k < F; k++) centres[k] = (double)(k * HOPV);
    std::vector<double> lag;
    fit_lag_curve(track_lags(am, bm, rate, c0.lag, 2.0, 1.0, 64, 512, 0, SIZE_MAX, (long long)c0.pos), centres,
                  rate, &lag);
    {
        std::vector<double> pos(n);
        for (size_t t = 0; t < n; t++) pos[t] = (double)t + interp1((double)t, centres, lag);
        std::vector<double> k1 = frac_read(bm, pos, 32, 9.0, CHUNKS);
        Track t2 = track_lags(am, k1, rate, 0, 2.0, 1.0, 16, 16);
        for (size_t i = 0; i < t2.lags.size(); i++) t2.lags[i] += interp1(t2.centres[i], centres, lag);
        rep.drift_line = fit_lag_curve(t2, centres, rate, &lag);
    }
    if (!step(30)) return {};
    const double cap = opt.quality ? std::min(opt.kvol, 1.5) : opt.kvol;
    // per-band sums over all frames, kept per chunk of frames and added in order
    using Sums = std::vector<std::vector<CVec>>;  // [chunk][channel][bin]
    auto zero_sums = [&] { return Sums(CHUNKS, std::vector<CVec>(C, CVec(NB, 0.0))); };
    auto total = [&](const Sums& s, size_t c) {
        CVec t(NB, 0.0);
        for (const auto& part : s)
            for (size_t b = 0; b < NB; b++) t[b] += part[c][b];
        return t;
    };
    // An EQ difference has a phase response (a low shelf delays the low band) that the
    // refinement would read as timing, so it compares the mix with the karaoke through the
    // EQ's phase (proto._eq_phase): a first per-band estimate (one pass, least squares,
    // 1/3 octave) without its linear part, which is a pure delay and must stay visible
    std::vector<CVec> H0;
    auto eq_phase = [&]() {
        Sums n0 = zero_sums(), d0 = zero_sums();
        parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            st.frame(kar, k, lag[k], &B);
            for (size_t c = 0; c < C; c++)
                for (size_t b = 0; b < NB; b++)
                    n0[t][c][b] += A[c][b] * std::conj(B[c][b]), d0[t][c][b] += std::norm(B[c][b]);
        });
        H0.assign(C, CVec(NB));
        for (size_t c = 0; c < C; c++) {
            CVec dc = total(d0, c);
            CVec sn = smooth_bins(total(n0, c), 1.0 / 3), sd = smooth_bins(dc, 1.0 / 3);
            double num = 0, den = 0;
            for (size_t b = 0; b < NB; b++) {
                H0[c][b] = sn[b] / (sd[b].real() + 1e-20);
                double fhz = (double)b * rate / (double)N;
                if (!(fhz > 100 && fhz < 6000)) continue;
                double om = 2 * PI * fhz / rate, w = dc[b].real();
                num += w * om * std::arg(H0[c][b]);
                den += w * om * om;
            }
            double tau = -num / den;
            for (size_t b = 0; b < NB; b++)
                H0[c][b] = std::exp(cplx(0, std::arg(H0[c][b] * std::exp(cplx(0, 2 * PI * (double)b / (double)N * tau)))));
        }
    };
    // per-frame refinement (proto.refine_frames): the phase slope of each frame's
    // cross-spectrum, bins weighted by how much the instrumental dominates them
    auto refine = [&](double f_hi) {
        std::vector<double> num(F), den(F);
        parallel_for(F, CHUNKS, [&](size_t k, size_t) {
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            st.frame(kar, k, lag[k], &B);
            double nu = 0, de = 0;
            for (size_t b = 0; b < NB; b++) {
                double fhz = (double)b * rate / (double)N;
                if (!(fhz > 100 && fhz < f_hi)) continue;
                cplx x = 0;
                double pb = 0, pa = 0;
                for (size_t c = 0; c < C; c++) {
                    cplx hb = H0[c][b] * B[c][b];
                    x += A[c][b] * std::conj(hb);
                    pb += std::norm(hb);
                    pa += std::norm(A[c][b]);
                }
                double om = 2 * PI * fhz / rate, w = pb * pb / (pa + pb + 1e-30);
                nu += w * om * std::arg(x);
                de += w * om * om;
            }
            num[k] = nu;
            den[k] = de + 1e-30;
        });
        double md = median(den) + 1e-30;
        std::vector<double> cd(F), conf(F), delta(F);
        for (size_t k = 0; k < F; k++) conf[k] = den[k] / md, cd[k] = conf[k] * (num[k] / den[k]);
        std::vector<double> s1 = gauss1d(cd, 3.0), s2 = gauss1d(conf, 3.0);
        for (size_t k = 0; k < F; k++) delta[k] = s1[k] / (s2[k] + 0.05);
        return delta;
    };
    // A wobble faster than the windows (a 33 rpm record: 1.8 s) averages out in them and
    // can pass for a straight line, so the first refinement always runs; the line is
    // kept when that finds nothing (median correction under 0.75 samples)
    eq_phase();
    std::vector<double> d1 = refine(2000.0);
    if (!step(38)) return {};
    std::vector<double> inner = F > 100 ? std::vector<double>(d1.begin() + 40, d1.end() - 40) : d1;
    for (double& v : inner) v = std::fabs(v);
    if (!rep.drift_line || median(inner) >= 0.75) {
        rep.drift_line = false;
        for (size_t k = 0; k < F; k++) lag[k] += d1[k];
        eq_phase();  // re-estimated once the first pass has taken out most of a wobble
        std::vector<double> d2 = refine(6000.0);
        for (size_t k = 0; k < F; k++) lag[k] += d2[k];
        if (!step(46)) return {};
    }
    rep.lag_start = lag.front();
    rep.lag_end = lag.back();

    // Frames are cut from the karaoke at one lag each (proto._stft_along): exact while
    // the lag holds still over a frame. When it moves by more than 0.6 samples across one
    // (fast drift, wow; proto.MAX_STRETCH) the karaoke is resampled along the curve instead.
    Planar kres;
    {
        const size_t d = N / HOPV;
        std::vector<double> sp;
        for (size_t k = d; k < F; k++) sp.push_back(std::fabs(lag[k] - lag[k - d]));
        rep.stretch = sp.empty() ? 0.0 : median(sp);
    }
    rep.resampled = rep.stretch > 0.6;
    if (rep.resampled) {
        std::vector<double> pos(n);
        for (size_t t = 0; t < n; t++) pos[t] = (double)t + interp1((double)t, centres, lag);
        for (const auto& ch : kar) kres.push_back(frac_read(ch, pos, 64, 10.0, CHUNKS));
    }
    auto frame_kar = [&](size_t k, std::vector<CVec>* B) {
        if (rep.resampled)
            st.frame(kres, k, 0.0, B);
        else
            st.frame(kar, k, lag[k], B);
    };

    // --- per-band EQ: two passes, the second over the cells the instrumental dominates
    Sums num = zero_sums(), den = zero_sums(), pa = zero_sums();
    parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
        std::vector<CVec> A, B;
        st.frame(mix_in, k, 0.0, &A);
        frame_kar(k, &B);
        for (size_t c = 0; c < C; c++)
            for (size_t b = 0; b < NB; b++)
                num[t][c][b] += A[c][b] * std::conj(B[c][b]), den[t][c][b] += std::norm(B[c][b]);
    });
    std::vector<CVec> H(C, CVec(NB));
    for (size_t c = 0; c < C; c++) {
        CVec sn = smooth_bins(total(num, c), opt.octave), sd = smooth_bins(total(den, c), opt.octave);
        for (size_t b = 0; b < NB; b++) H[c][b] = sn[b] / (sd[b].real() + 1e-20);
    }
    num = zero_sums(), den = zero_sums();
    if (!step(58)) return {};
    parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
        std::vector<CVec> A, B;
        st.frame(mix_in, k, 0.0, &A);
        frame_kar(k, &B);
        for (size_t c = 0; c < C; c++)
            for (size_t b = 0; b < NB; b++)
                if (std::abs(H[c][b] * B[c][b]) * cap > std::abs(A[c][b])) {
                    num[t][c][b] += A[c][b] * std::conj(B[c][b]);
                    den[t][c][b] += std::norm(B[c][b]);
                    pa[t][c][b] += std::norm(A[c][b]);
                }
    });
    for (size_t c = 0; c < C; c++) {
        CVec sn = smooth_bins(total(num, c), opt.octave), sd = smooth_bins(total(den, c), opt.octave),
             sp = smooth_bins(total(pa, c), opt.octave);
        for (size_t b = 0; b < NB; b++) {
            double d = sd[b].real() + 1e-20;
            H[c][b] = std::sqrt(sp[b].real() / d) * std::exp(cplx(0, std::arg(sn[b] / d)));
        }
    }
    if (!step(70)) return {};

    // --- per-frame level correction (proto._level_track), in three passes, each
    // smoothed over frames: least squares over all cells, then over the cells the
    // corrected estimate says the instrumental dominates, then |gain| from those cells'
    // power ratio. Auto: estimated on the even bins, it must cut the residual on the odd
    // bins (cells the uncorrected estimate says the instrumental dominates) by lvl_gain.
    std::vector<std::vector<double>> corr(C, std::vector<double>(F, 1.0));
    bool apply_level = false;
    if (opt.level_track) {
        using Curves = std::vector<std::vector<double>>;  // [channel][frame]
        auto zeros = [&] { return Curves(C, std::vector<double>(F, 0.0)); };
        // smoothed nu / de, 1 where de vanishes; `root`: the square root, unclipped
        auto ratio = [&](const std::vector<double>& nu, const std::vector<double>& de, bool root) {
            std::vector<double> sn = gauss1d(nu, 2.0), sd = gauss1d(de, 2.0), c(F);
            for (size_t k = 0; k < F; k++) {
                if (!(sd[k] > 1e-12)) {
                    c[k] = 1.0;
                    continue;
                }
                double v = sn[k] / (sd[k] + 1e-20);
                c[k] = root ? std::sqrt(v) : std::clamp(v, 0.25, 4.0);
            }
            return c;
        };
        // set 0: all bins (the correction), set 1: even bins (the out-of-sample test)
        Curves g[2] = {zeros(), zeros()};  // the correction so far
        for (auto& x : g)
            for (auto& ch : x) std::fill(ch.begin(), ch.end(), 1.0);
        Curves c0[2] = {zeros(), zeros()}, c1[2] = {zeros(), zeros()};
        Curves oa = zeros(), ox = zeros(), oi = zeros();
        for (int step = 0; step < 3; step++) {
            Curves nu[2] = {zeros(), zeros()}, de[2] = {zeros(), zeros()};
            parallel_for(F, CHUNKS, [&](size_t k, size_t) {
                std::vector<CVec> A, B;
                st.frame(mix_in, k, 0.0, &A);
                frame_kar(k, &B);
                for (size_t c = 0; c < C; c++)
                    for (size_t b = 0; b < NB; b++) {
                        cplx I = H[c][b] * B[c][b];
                        double aa = std::norm(A[c][b]);
                        if (step == 0 && b % 2 == 1 && std::abs(I) * cap > std::abs(A[c][b])) {
                            double x = (A[c][b] * std::conj(I)).real();
                            oa[c][k] += aa, ox[c][k] += x, oi[c][k] += std::norm(I);
                        }
                        for (int set = 0; set < 2; set++) {
                            if (set == 1 && b % 2) continue;
                            cplx X = I * g[set][c][k];
                            if (step > 0 && !(std::abs(X) * cap > std::abs(A[c][b]))) continue;
                            if (step < 2)
                                nu[set][c][k] += (A[c][b] * std::conj(X)).real(), de[set][c][k] += std::norm(X);
                            else
                                nu[set][c][k] += aa, de[set][c][k] += std::norm(X);
                        }
                    }
            });
            for (int set = 0; set < 2; set++)
                for (size_t c = 0; c < C; c++) {
                    std::vector<double> f = ratio(nu[set][c], de[set][c], step == 2);
                    if (step == 0) {
                        c0[set][c] = g[set][c] = f;
                    } else if (step == 1) {
                        c1[set][c] = f;
                        for (size_t k = 0; k < F; k++) g[set][c][k] = c0[set][c][k] * f[k];
                    } else {  // c0 * clip(c1 * r)
                        for (size_t k = 0; k < F; k++)
                            g[set][c][k] = std::clamp(c0[set][c][k] * std::clamp(c1[set][c][k] * f[k], 0.25, 4.0), 0.25, 4.0);
                    }
                }
        }
        double r0 = 0, r1 = 0;
        for (size_t c = 0; c < C; c++) {
            corr[c] = g[0][c];
            const std::vector<double>& ce = g[1][c];
            for (size_t k = 0; k < F; k++) {
                r0 += oa[c][k] - 2 * ox[c][k] + oi[c][k];
                r1 += oa[c][k] - 2 * ce[k] * ox[c][k] + ce[k] * ce[k] * oi[c][k];
            }
        }
        rep.level_gain = r0 > 1e-30 ? 1 - r1 / r0 : 0.0;  // nothing to correct in silence
        apply_level = opt.level_track == 1 || rep.level_gain > opt.lvl_gain;
    }
    rep.level_applied = apply_level;
    if (!step(78)) return {};

    // --- subtraction with 3.0's decision rule, inverse STFT (scipy.signal.istft)
    std::vector<double> thr(NB);
    for (size_t b = 0; b < NB; b++) thr[b] = opt.kvol * std::max(PI * ((double)b + 1.0) / (double)(N / 2), 0.15);
    size_t ext = (F - 1) * HOPV + N;
    Planar acc(C, std::vector<double>(ext, 0.0));
    std::vector<double> norm(ext, 0.0);
    const size_t BLOCK = 256;
    std::vector<CVec> zs(BLOCK, CVec(N));
    for (size_t k0 = 0; k0 < F; k0 += BLOCK) {
        size_t cnt = std::min(BLOCK, F - k0);
        parallel_for(cnt, CHUNKS, [&](size_t i, size_t) {
            size_t k = k0 + i;
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            frame_kar(k, &B);
            std::vector<CVec> V(C, CVec(NB));
            for (size_t c = 0; c < C; c++) {
                double g = apply_level ? corr[c][k] : 1.0;
                for (size_t b = 0; b < NB; b++) {
                    cplx I = H[c][b] * B[c][b] * g;
                    bool kill = std::abs(I) * cap > std::abs(A[c][b]);
                    if (kill && opt.quality) kill = phase_diff(A[c][b], I) < thr[b];
                    V[c][b] = kill ? cplx(0.0) : A[c][b] - I;
                }
            }
            // both channels in one inverse FFT: Hermitian(L) + i Hermitian(R)
            auto herm = [&](const CVec& v, size_t b) -> cplx {
                if (b == 0) return cplx(v[0].real(), 0.0);
                if (b == N / 2) return cplx(v[N / 2].real(), 0.0);
                return b < N / 2 ? v[b] : std::conj(v[N - b]);
            };
            CVec& z = zs[i];
            for (size_t b = 0; b < N; b++) {
                cplx l = herm(V[0], b) * st.wsum;
                cplx r = C > 1 ? herm(V[1], b) * st.wsum : cplx(0.0);
                z[b] = l + cplx(0, 1) * r;
            }
            st.fft.inverse(z.data());
        });
        for (size_t i = 0; i < cnt; i++) {
            size_t k = k0 + i;
            const CVec& z = zs[i];
            for (size_t j = 0; j < N; j++) {
                size_t t = k * HOPV + j;
                double w = st.win[j];
                acc[0][t] += z[j].real() / (double)N * w;
                if (C > 1) acc[1][t] += z[j].imag() / (double)N * w;
                norm[t] += w * w;
            }
        }
        if (!step(78 + (int)(20 * (k0 + cnt) / F))) return {};
    }
    Planar out(C, std::vector<double>(n));
    for (size_t c = 0; c < C; c++)
        for (size_t t = 0; t < n; t++) {
            size_t e = t + N / 2;
            out[c][t] = acc[c][e] / (norm[e] > 1e-10 ? norm[e] : 1.0);
        }
    if (report) *report = rep;
    step(100);
    return out;
}

}  // namespace v4
}  // namespace utagoe
