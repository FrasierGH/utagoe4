// Utagoe Rip 4 separation. Follows eval/proto.py (engine `v4`) step by step; where the
// prototype holds whole spectrograms, this streams STFT frames in several passes. What
// stays in memory is a few full-length copies of the signals (about 2.5 GB at the peak
// for a 10-minute stereo song, the caller's copies included).
#include "v4.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstdlib>
#include <exception>
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
const double MAX_LAG_SEC = 30.0;  // the karaoke may start up to this much earlier or later

// a band edge in Hz, kept below Nyquist at low sample rates
double band_hi(double hz, double rate) { return std::min(hz, 0.45 * rate); }

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
    // an exception (bad_alloc) must not escape a thread (std::terminate): the first one
    // is rethrown here, on the caller's thread
    std::vector<std::exception_ptr> err(chunks);
    std::vector<std::thread> th;
    for (size_t t = 0; t < chunks; t++)
        th.emplace_back([&, t] {
            try {
                for (size_t i = count * t / chunks; i < count * (t + 1) / chunks; i++) fn(i, t);
            } catch (...) {
                err[t] = std::current_exception();
            }
        });
    for (auto& x : th) x.join();
    for (auto& e : err)
        if (e) std::rethrow_exception(e);
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
        if (it == cache.end()) it = cache.emplace(rate, butter4_bandpass(100.0, band_hi(8000.0, rate), rate)).first;
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
        if (it == cache.end()) it = cache.emplace(rate, butter4_lowpass(band_hi(1000.0, rate), rate)).first;
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
            if (!(fhz > 100 && fhz < band_hi(8000.0, rate))) continue;
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
                   double sigma_sec = 1.5, double const_lag = 0.0) {
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
        double span = *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
        // the drift's standard error, from the weighted residuals of the line
        double sr = 0, sc = 0;
        for (size_t i = 0; i < c.size(); i++) {
            double r = slope * c[i] + icept - y[i];
            sr += w[i] * r * r, sc += w[i] * (c[i] - xm) * (c[i] - xm);
        }
        double se_drift = std::sqrt(sr / sw) / (std::sqrt(sc / sw) * std::sqrt((double)std::max<size_t>(c.size(), 3) - 2) + 1e-30) * span;
        if (const_lag > 0 && std::fabs(slope * span) < std::max(const_lag, 3 * se_drift)) {
            // no drift to speak of (identical clocks): a constant, without the slope's
            // noise. Under 3 standard errors also counts: an EQ difference biases each
            // window's fraction by an amount that follows the music, which can fake a
            // small drift (proto.fit_lag_curve)
            double sy0 = 0, sw0 = 0;
            for (size_t i = 0; i < c.size(); i++) sy0 += w[i] * y[i], sw0 += w[i];
            out->assign(at.size(), sy0 / sw0);
            return true;
        }
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
    parallel_for(count, std::min<size_t>(count, 4), [&](size_t i, size_t) {  // ~100 MB each
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
                            double max_sec = MAX_LAG_SEC, size_t candidates = 6, long long sep = 256, double excerpt_sec = 60.0) {
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
                  double excerpt_sec = 60.0, long long same = 512, double max_sec = MAX_LAG_SEC) {
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
    // the windows look for drift and nearby repeats, within +-10 s
    long long mw = std::min(m, (long long)(10.0 * rate));
    std::vector<Cand> extra = rank(lowpass(a, rate), lowpass(b, rate), window_peaks(a, b, rate, mw, 256));
    size_t added = 0;
    for (const Cand& c : extra) {
        if (added == top_windows) break;
        if (add(c)) added++;
    }
    size_t E = std::min(n, (size_t)(excerpt_sec * rate)), e0 = (n - E) / 2, mid = e0 + E / 2;
    if (cands.empty()) return {0, 1, mid};
    std::vector<double> ab = bandpass(std::vector<double>(a.begin() + (long long)e0, a.begin() + (long long)(e0 + E)), rate);
    std::vector<double> sub;
    for (size_t t = e0; t < e0 + E; t += 256) sub.push_back((double)t);
    std::vector<double> res(cands.size()), gain(cands.size()), lag_mid(cands.size());
    parallel_for(cands.size(), std::min<size_t>(cands.size(), 6), [&](size_t j, size_t) {
        Track tr = track_lags(a, b, rate, cands[j].lag, 2.0, 1.0, 64, 512, e0, e0 + E, (long long)mid);
        std::vector<double> curve;
        fit_lag_curve(tr, sub, rate, &curve);
        std::vector<double> pos(E);
        for (size_t i = 0; i < E; i++) pos[i] = (double)(e0 + i) + interp1((double)(e0 + i), sub, curve);
        std::vector<double> k = bandpass(frac_read(b, pos, 16), rate);
        std::tie(res[j], gain[j]) = trial(ab, k);
        lag_mid[j] = interp1((double)mid, sub, curve);
    });
    // The best trial wins; candidates within 5 % of it (a song that repeats itself, say)
    // are told apart by how much of the two files overlaps at their lag
    double best_res = *std::min_element(res.begin(), res.end());
    auto overlap = [&](double lag) {
        double lo = std::max(0.0, -lag), hi = std::min((double)n, (double)b.size() - lag);
        return std::max(0.0, hi - lo);
    };
    size_t win = 0;
    double win_ov = -1;
    for (size_t j = 0; j < cands.size(); j++)
        if (res[j] <= best_res * 1.05 && overlap(lag_mid[j]) > win_ov) win = j, win_ov = overlap(lag_mid[j]);
    return {(long long)std::nearbyint(lag_mid[win]), gain[win] >= 0 ? 1 : -1, mid};
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
                const std::function<bool(int)>& progress, Planar* instrumental) {
    auto step = [&](int pct) { return !progress || progress(pct); };
    const double rate = (double)rate_i;
    const size_t n = mix_in[0].size(), C = mix_in.size();
    Report rep;
    bool silent = true;  // an all-zero karaoke has nothing to align either
    for (const auto& ch : kar_in)
        for (double v : ch) silent = silent && v == 0.0;
    if (n == 0 || kar_in[0].empty() || silent) {  // nothing to align: the mix as it is
        if (report) *report = rep;
        if (instrumental) *instrumental = Planar(C, std::vector<double>(n, 0.0));  // nothing subtracted
        return mix_in;
    }

    // Only the karaoke up to MAX_LAG_SEC past the mix's end can line up with it (the
    // lag search covers +-MAX_LAG_SEC), so a much longer one is trimmed
    size_t kn = std::min(kar_in[0].size(), n + (size_t)(MAX_LAG_SEC * rate) + N);
    Planar kar(C);
    for (size_t c = 0; c < C; c++) kar[c].assign(kar_in[c].begin(), kar_in[c].begin() + (long long)kn);

    // --- global lag and polarity, from the best tracked candidate
    std::vector<double> am = mono(mix_in), bm = mono(kar);
    Pick c0 = pick_tracked(am, bm, rate);
    rep.lag = (long)c0.lag;
    rep.sign = c0.sign;
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
                  rate, &lag, 1.5, opt.const_lag);
    {
        std::vector<double> pos(n);
        for (size_t t = 0; t < n; t++) pos[t] = (double)t + interp1((double)t, centres, lag);
        std::vector<double> k1 = frac_read(bm, pos, 32, 9.0, CHUNKS);
        Track t2 = track_lags(am, k1, rate, 0, 2.0, 1.0, 16, 16);
        for (size_t i = 0; i < t2.lags.size(); i++) t2.lags[i] += interp1(t2.centres[i], centres, lag);
        rep.drift_line = fit_lag_curve(t2, centres, rate, &lag, 1.5, opt.const_lag);
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
                if (!(fhz > 100 && fhz < band_hi(6000.0, rate))) continue;
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
    std::vector<double> d1 = refine(band_hi(2000.0, rate));
    if (!step(38)) return {};
    std::vector<double> inner = F > 100 ? std::vector<double>(d1.begin() + 40, d1.end() - 40) : d1;
    for (double& v : inner) v = std::fabs(v);
    if (!rep.drift_line || median(inner) >= 0.75) {
        rep.drift_line = false;
        // A wobble the coarse windows missed can leave errors past the 2 kHz band's
        // unambiguous range (about 11 samples); a start below 500 Hz (about 44 samples)
        // reaches them, but where there is nothing to reach it only adds noise. So both
        // paths run to the end and the low-band one is kept where the instrumental fits
        // clearly better along it (below).
        const std::vector<double> base = lag;
        std::vector<double> d0;
        if (opt.wow) d0 = refine(500.0);
        auto finish = [&](const std::vector<double>& d) {
            for (size_t k = 0; k < F; k++) lag[k] += d[k];
            eq_phase();  // re-estimated once the first pass has taken out most of a wobble
            std::vector<double> d2 = refine(band_hi(6000.0, rate));
            for (size_t k = 0; k < F; k++) lag[k] += d2[k];
        };
        finish(d1);
        if (!step(40)) return {};
        if (opt.wow) {
            std::vector<double> la = lag;
            lag = base;
            for (size_t k = 0; k < F; k++) lag[k] += d0[k];
            eq_phase();
            std::vector<double> e1 = refine(band_hi(2000.0, rate));
            if (!step(42)) return {};
            finish(e1);
            if (!step(44)) return {};
            std::vector<double> lb = lag;
            // Judged on the cells the instrumental dominates along the usual path (the
            // vocal stays out of it), each path with its own EQ (least squares, 1/3 octave):
            // the low-band path must leave under 0.9 of the residual
            std::vector<CVec> Ha(C), Hb(C);
            {
                Sums na = zero_sums(), da = zero_sums(), nb = zero_sums(), db = zero_sums();
                parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
                    std::vector<CVec> A, Ba, Bb;
                    st.frame(mix_in, k, 0.0, &A);
                    st.frame(kar, k, la[k], &Ba);
                    st.frame(kar, k, lb[k], &Bb);
                    for (size_t c = 0; c < C; c++)
                        for (size_t b = 0; b < NB; b++) {
                            na[t][c][b] += A[c][b] * std::conj(Ba[c][b]), da[t][c][b] += std::norm(Ba[c][b]);
                            nb[t][c][b] += A[c][b] * std::conj(Bb[c][b]), db[t][c][b] += std::norm(Bb[c][b]);
                        }
                });
                for (size_t c = 0; c < C; c++) {
                    CVec sna = smooth_bins(total(na, c), 1.0 / 3), sda = smooth_bins(total(da, c), 1.0 / 3);
                    CVec snb = smooth_bins(total(nb, c), 1.0 / 3), sdb = smooth_bins(total(db, c), 1.0 / 3);
                    Ha[c].resize(NB), Hb[c].resize(NB);
                    for (size_t b = 0; b < NB; b++) {
                        Ha[c][b] = sna[b] / (sda[b].real() + 1e-20);
                        Hb[c][b] = snb[b] / (sdb[b].real() + 1e-20);
                    }
                }
            }
            std::vector<double> ra(CHUNKS, 0.0), rb(CHUNKS, 0.0);
            parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
                std::vector<CVec> A, Ba, Bb;
                st.frame(mix_in, k, 0.0, &A);
                st.frame(kar, k, la[k], &Ba);
                st.frame(kar, k, lb[k], &Bb);
                for (size_t c = 0; c < C; c++)
                    for (size_t b = 0; b < NB; b++) {
                        cplx ia = Ha[c][b] * Ba[c][b];
                        if (!(std::abs(ia) * cap > std::abs(A[c][b]))) continue;
                        ra[t] += std::norm(A[c][b] - ia);
                        rb[t] += std::norm(A[c][b] - Hb[c][b] * Bb[c][b]);
                    }
            });
            double sa = 0, sb = 0;
            for (size_t t = 0; t < CHUNKS; t++) sa += ra[t], sb += rb[t];
            rep.wow_lowband = sb < 0.9 * sa;
            lag = rep.wow_lowband ? lb : la;
        }
        if (!step(46)) return {};
    }
    rep.lag_start = lag.front();
    rep.lag_end = lag.back();
    std::vector<double>().swap(am);  // the mono sums were for the alignment only
    std::vector<double>().swap(bm);

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
        Planar().swap(kar);  // only the resampled karaoke is read from here on
    }
    auto frame_kar = [&](size_t k, std::vector<CVec>* B) {
        if (rep.resampled)
            st.frame(kres, k, 0.0, B);
        else
            st.frame(kar, k, lag[k], B);
    };

    // --- per-band EQ: least squares over all cells, then either Huber-weighted least
    // squares (proto._estimate_h_huber) or 4.0's second pass over the cells the
    // instrumental dominates
    Sums num = zero_sums(), den = zero_sums(), pa = zero_sums();
    parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
        std::vector<CVec> A, B;
        st.frame(mix_in, k, 0.0, &A);
        frame_kar(k, &B);
        for (size_t c = 0; c < C; c++)
            for (size_t b = 0; b < NB; b++) {
                num[t][c][b] += A[c][b] * std::conj(B[c][b]), den[t][c][b] += std::norm(B[c][b]);
                pa[t][c][b] += std::norm(A[c][b]);
            }
    });
    // Lossy coding (proto.lossy): a coder's low-pass in either file, a drop of more than
    // 20 dB within 1 kHz somewhere between 11 and 20.5 kHz of the mean spectrum
    auto lossy = [&](const Sums& s) {
        std::vector<double> p(NB, 0.0);
        for (size_t c = 0; c < C; c++) {
            CVec t = total(s, c);
            for (size_t b = 0; b < NB; b++) p[b] += t[b].real() / (double)(C * F);
        }
        const double lo = 11000.0, step = 250.0, stop = std::min(20500.0, rate / 2) + 1e-9;
        size_t ne = stop > lo ? (size_t)std::ceil((stop - lo) / step) : 0;
        std::vector<double> lvl(ne);
        for (size_t e = 0; e < ne; e++) {
            double f0 = lo + (double)e * step, sum = 0;
            size_t cnt = 0;
            for (size_t b = 0; b < NB; b++) {
                double fhz = (double)b * rate / (double)N;
                if (fhz >= f0 && fhz < f0 + step) sum += p[b], cnt++;
            }
            lvl[e] = 10 * std::log10((cnt ? sum / (double)cnt : 0.0) + 1e-30);
        }
        const size_t kk = 4;  // 1 kHz
        double drop = -INFINITY;
        for (size_t e = 0; e + kk < ne; e++) drop = std::max(drop, lvl[e] - lvl[e + kk]);
        return ne > kk && drop > 20.0;
    };
    const bool coded = lossy(pa) || lossy(den);
    // A band-limited karaoke (a rip with a low-pass the original lacks): above its cut the
    // karaoke has nothing, so nothing there can be subtracted. Bins where its mean power is
    // 20 dB under its usual share of the mix's (1-8 kHz), those where the mix itself is
    // silent (-50 dB) counting either way, median of 9; the top run of them, gaps under
    // 500 Hz bridged, starting below 20 kHz, 80% such bins, with at least 1 kHz of them
    // where the mix is not silent, and a cliff in the karaoke's own spectrum (below).
    size_t top_b0 = NB;  // the run's first bin (NB: none)
    if (opt.noref) {
        std::vector<double> ma(NB, 0.0), mb(NB, 0.0), mid_r, mid_a;
        for (size_t c = 0; c < C; c++) {
            CVec ta = total(pa, c), tb = total(den, c);
            for (size_t b = 0; b < NB; b++) ma[b] += ta[b].real(), mb[b] += tb[b].real();
        }
        std::vector<double> r(NB);
        for (size_t b = 0; b < NB; b++) {
            ma[b] /= (double)(C * F), mb[b] /= (double)(C * F);
            r[b] = mb[b] / (ma[b] + 1e-30);
            double fhz = (double)b * rate / (double)N;
            if (fhz > 1000 && fhz < 8000) mid_r.push_back(r[b]), mid_a.push_back(ma[b]);
        }
        const double lim = 1e-2 * median(mid_r), quiet = 1e-5 * median(mid_a);
        std::vector<char> low(NB), dead(NB), nr(NB);
        for (size_t b = 0; b < NB; b++) low[b] = r[b] < lim, dead[b] = ma[b] < quiet;
        for (long long b = 0; b < (long long)NB; b++) {  // scipy median_filter, mode 'reflect'
            int cnt = 0;
            for (long long j = b - 4; j <= b + 4; j++) {
                long long i = j < 0 ? -j - 1 : j >= (long long)NB ? 2 * (long long)NB - j - 1 : j;
                cnt += low[(size_t)i] || dead[(size_t)i];
            }
            nr[(size_t)b] = cnt >= 5;
        }
        const long long gap = (long long)std::ceil(500.0 * N / rate);
        long long last = (long long)NB;
        for (long long b = (long long)NB - 1; b >= 0; b--) {
            if (nr[(size_t)b])
                last = b;
            else if (last - b > gap)
                break;
        }
        if (last < (long long)NB) {
            size_t b0 = (size_t)last, cnt = 0, live = 0;
            for (size_t b = b0; b < NB; b++) cnt += nr[b], live += low[b] && !dead[b];
            double f0 = (double)b0 * rate / (double)N;
            // and the karaoke's own spectrum falls off a cliff there (20 dB from the 750 Hz
            // below the cut to the 750 Hz above it, 250 Hz either side left out): a dark
            // arrangement under a bright vocal also leaves the karaoke far under the mix up
            // there, but its roll-off is gradual
            double plo = 0, phi = 0;
            size_t nlo = 0, nhi = 0;
            for (size_t b = 0; b < NB; b++) {
                double fhz = (double)b * rate / (double)N;
                if (fhz >= f0 - 1000 && fhz < f0 - 250) plo += mb[b], nlo++;
                if (fhz >= f0 + 250 && fhz < f0 + 1000) phi += mb[b], nhi++;
            }
            bool cliff = nlo && nhi && plo / (double)nlo > 100 * (phi / (double)nhi);
            if (f0 < 20000 && (double)cnt / (double)(NB - b0) >= 0.8 && (double)live * rate / (double)N >= 1000 && cliff)
                top_b0 = b0;
        }
    }
    rep.lowpass_hz = top_b0 < NB ? (double)top_b0 * rate / (double)N : 0.0;
    pa = zero_sums();
    std::vector<CVec> H(C, CVec(NB));
    for (size_t c = 0; c < C; c++) {
        CVec sn = smooth_bins(total(num, c), opt.octave), sd = smooth_bins(total(den, c), opt.octave);
        for (size_t b = 0; b < NB; b++) H[c][b] = sn[b] / (sd[b].real() + 1e-20);
    }
    num = zero_sums(), den = zero_sums();
    if (!step(58)) return {};
    // The 2x2 EQ (stereo; proto._estimate_h_mimo): each output channel from both karaoke
    // channels, for a karaoke whose stereo image differs from the mix's (another width or
    // balance). Hm[c * 2 + d]: from karaoke channel d into channel c.
    bool use_mimo = false;
    std::vector<CVec> Hm;
    if (opt.huber) {
        // Huber-weighted least squares (proto._estimate_h_huber): each iteration, the median
        // of |A - H B| per bin over the frames (from a histogram of log10 |r|, as
        // proto._hist_median), then least squares with weights min(1, 1.5 median / |r|).
        // Fitted side by side, in the same passes over the frames: [0] the per-channel EQ
        // over all frames, and for stereo also [1] the same on the even frames, [2] the 2x2
        // EQ on the even frames and [3] over all frames. The 2x2 EQ is used where, fitted on
        // the even frames, it predicts the odd ones (the cells the instrumental dominates)
        // with at least 20% less residual than the per-channel one; with the image the same
        // its extra freedom only adds estimation noise.
        struct Eq {
            bool mimo, even;
            std::vector<CVec> H;
        };
        std::vector<Eq> eqs = {{false, false, H}};
        if (opt.mimo && C == 2) {
            eqs.push_back({false, true, std::vector<CVec>(C, CVec(NB))});
            eqs.push_back({true, true, std::vector<CVec>(4, CVec(NB))});
            eqs.push_back({true, false, std::vector<CVec>(4, CVec(NB))});
        }
        const size_t M = eqs.size();
        auto predict_m = [&](const Eq& e, const std::vector<CVec>& B, size_t c, size_t b) -> cplx {
            return e.mimo ? e.H[c * 2][b] * B[0][b] + e.H[c * 2 + 1][b] * B[1][b] : e.H[c][b] * B[c][b];
        };
        const size_t NHB = 700;
        std::vector<double> hedges(NHB + 1);
        for (size_t e = 0; e < NHB; e++) hedges[e] = (double)e * (14.0 / (double)NHB) + -12.0;
        hedges[NHB] = 2.0;
        auto hbin = [&](double r) {
            double v = std::clamp(std::log10(r + 1e-300), hedges[0], hedges[NHB] - 1e-9);
            return (size_t)(std::upper_bound(hedges.begin(), hedges.end(), v) - hedges.begin()) - 1;
        };
        const size_t BLK = 64, CB = C * NB;
        std::vector<std::vector<double>> delta(M, std::vector<double>(CB, 0.0));
        // weighted least squares for every model but those in `skip`; unweighted on the
        // first call. Sums per channel: per-channel EQ nu, de; 2x2 EQ p0, p1, R00, R11, R01.
        auto fit = [&](bool first, size_t skip) {
            std::vector<std::vector<std::vector<CVec>>> acc(M);  // [model][chunk][slot][bin]
            for (size_t m = 0; m < M; m++)
                if (m >= skip) acc[m].assign(CHUNKS, std::vector<CVec>(C * (eqs[m].mimo ? 5 : 2), CVec(NB, 0.0)));
            parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
                std::vector<CVec> A, B;
                st.frame(mix_in, k, 0.0, &A);
                frame_kar(k, &B);
                for (size_t m = skip; m < M; m++) {
                    const Eq& e = eqs[m];
                    if (e.even && k % 2) continue;
                    std::vector<CVec>& s = acc[m][t];
                    for (size_t c = 0; c < C; c++)
                        for (size_t b = 0; b < NB; b++) {
                            double w = first ? 1.0
                                             : std::min(1.0, delta[m][c * NB + b] /
                                                                 (std::abs(A[c][b] - predict_m(e, B, c, b)) + 1e-30));
                            if (!e.mimo) {
                                s[c][b] += w * (A[c][b] * std::conj(B[c][b]));
                                s[C + c][b] += w * std::norm(B[c][b]);
                            } else {
                                s[c * 5][b] += w * (A[c][b] * std::conj(B[0][b]));
                                s[c * 5 + 1][b] += w * (A[c][b] * std::conj(B[1][b]));
                                s[c * 5 + 2][b] += w * std::norm(B[0][b]);
                                s[c * 5 + 3][b] += w * std::norm(B[1][b]);
                                s[c * 5 + 4][b] += w * (B[0][b] * std::conj(B[1][b]));
                            }
                        }
                }
            });
            for (size_t m = skip; m < M; m++) {
                auto slot = [&](size_t i) {
                    CVec tt(NB, 0.0);
                    for (const auto& part : acc[m])
                        for (size_t b = 0; b < NB; b++) tt[b] += part[i][b];
                    return smooth_bins(tt, opt.octave);
                };
                Eq& e = eqs[m];
                for (size_t c = 0; c < C; c++) {
                    if (!e.mimo) {
                        CVec sn = slot(c), sd = slot(C + c);
                        for (size_t b = 0; b < NB; b++) e.H[c][b] = sn[b] / (sd[b].real() + 1e-20);
                        continue;
                    }
                    // A_c ~ h0 B0 + h1 B1: R^T h = p with R[d][e] = sum B_d conj(B_e), a ridge
                    // of 1e-6 of its trace on the diagonal
                    CVec p0 = slot(c * 5), p1 = slot(c * 5 + 1), r00 = slot(c * 5 + 2), r11 = slot(c * 5 + 3),
                         r01 = slot(c * 5 + 4);
                    for (size_t b = 0; b < NB; b++) {
                        double ridge = 1e-6 * (r00[b].real() + r11[b].real());
                        cplx a = r00[b] + ridge, d = r11[b] + ridge, r10 = std::conj(r01[b]);
                        cplx det = a * d - r10 * r01[b];
                        if (std::abs(det) == 0.0) {
                            e.H[c * 2][b] = e.H[c * 2 + 1][b] = 0.0;
                            continue;
                        }
                        e.H[c * 2][b] = (d * p0[b] - r10 * p1[b]) / det;
                        e.H[c * 2 + 1][b] = (a * p1[b] - r01[b] * p0[b]) / det;
                    }
                }
            }
        };
        if (M > 1) fit(true, 1);  // the per-channel EQ over all frames is H already
        std::vector<double> rbuf(M * BLK * CB);
        std::vector<uint32_t> hist(M * CB * NHB);
        for (int it = 0; it < opt.huber_iters; it++) {
            std::fill(hist.begin(), hist.end(), 0u);
            for (size_t k0 = 0; k0 < F; k0 += BLK) {
                size_t cnt = std::min(BLK, F - k0);
                parallel_for(cnt, CHUNKS, [&](size_t i, size_t) {
                    std::vector<CVec> A, B;
                    st.frame(mix_in, k0 + i, 0.0, &A);
                    frame_kar(k0 + i, &B);
                    for (size_t m = 0; m < M; m++)
                        for (size_t c = 0; c < C; c++)
                            for (size_t b = 0; b < NB; b++)
                                rbuf[(m * BLK + i) * CB + c * NB + b] = std::abs(A[c][b] - predict_m(eqs[m], B, c, b));
                });
                parallel_for(M * CB, CHUNKS, [&](size_t mq, size_t) {
                    size_t m = mq / CB, q = mq % CB;
                    for (size_t i = 0; i < cnt; i++)
                        if (!eqs[m].even || (k0 + i) % 2 == 0) hist[mq * NHB + hbin(rbuf[(m * BLK + i) * CB + q])]++;
                });
            }
            for (size_t m = 0; m < M; m++) {
                const uint64_t frames = eqs[m].even ? ((uint64_t)F + 1) / 2 : (uint64_t)F;
                const uint64_t need = (frames + 1) / 2;
                for (size_t q = 0; q < CB; q++) {
                    uint64_t cum = 0;
                    size_t e = 0;
                    for (; e < NHB; e++)
                        if ((cum += hist[(m * CB + q) * NHB + e]) >= need) break;
                    delta[m][q] = 1.5 * std::pow(10.0, hedges[std::min(e, NHB - 1) + 1]);
                }
            }
            fit(false, 0);
            if (!step(58 + 10 * (it + 1) / opt.huber_iters)) return {};
        }
        H = eqs[0].H;
        if (M > 1) {
            std::vector<double> rd(CHUNKS, 0.0), rm(CHUNKS, 0.0);
            parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
                if (k % 2 == 0) return;
                std::vector<CVec> A, B;
                st.frame(mix_in, k, 0.0, &A);
                frame_kar(k, &B);
                for (size_t c = 0; c < C; c++)
                    for (size_t b = 0; b < NB; b++) {
                        cplx id = predict_m(eqs[1], B, c, b);
                        if (!(std::abs(id) * cap > std::abs(A[c][b]))) continue;
                        rd[t] += std::norm(A[c][b] - id);
                        rm[t] += std::norm(A[c][b] - predict_m(eqs[2], B, c, b));
                    }
            });
            double sd = 0, sm = 0;
            for (size_t t = 0; t < CHUNKS; t++) sd += rd[t], sm += rm[t];
            use_mimo = sm < (1 - 0.2) * sd;
            rep.mimo = use_mimo;
            if (use_mimo) Hm = std::move(eqs[3].H);
        }
        hist = std::vector<uint32_t>();
    } else {
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
    }
    if (!step(70)) return {};
    auto predict = [&](const std::vector<CVec>& B, size_t c, size_t b) -> cplx {
        return use_mimo ? Hm[c * 2][b] * B[0][b] + Hm[c * 2 + 1][b] * B[1][b] : H[c][b] * B[c][b];
    };

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
        const size_t EDGE = N / HOPV / 2;  // frames reaching past an end of the mix
        for (int step = 0; step < 3; step++) {
            Curves nu[2] = {zeros(), zeros()}, de[2] = {zeros(), zeros()};
            parallel_for(F, CHUNKS, [&](size_t k, size_t) {
                std::vector<CVec> A, B;
                st.frame(mix_in, k, 0.0, &A);
                frame_kar(k, &B);
                for (size_t c = 0; c < C; c++)
                    for (size_t b = 0; b < NB; b++) {
                        cplx I = predict(B, c, b);
                        double aa = std::norm(A[c][b]);
                        if (step == 0 && b % 2 == 1 && k >= EDGE && k + EDGE < F &&
                            std::abs(I) * cap > std::abs(A[c][b])) {
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

    // --- soft decision (proto._soft_gain): the model error rho per band of 1/3 octave, a
    // quantile of log10 |A - I|^2 / |I|^2 over the cells the instrumental dominates; then
    // each cell keeps max(0, 1 - strength rho |I|^2 / |A - I|^2) of A - I. The quantile is
    // low (0.10) where the model can be exact (no level tracking, a lag that is a line, no
    // lossy coding) and the median where the releases' dynamics differ, the lag wobbles or
    // a lossy coder's noise differs between the files. (With level tracking turned off,
    // opt.level_track = 0, only the lag and the coding count.)
    std::vector<size_t> sband(NB);
    size_t nbands = 0;
    std::vector<std::vector<double>> rho;
    const double strength = opt.kvol / 1.2 * (opt.quality ? 1.0 : 2.0);
    if (opt.soft) {
        std::vector<double> edges = {0.0, 50.0};
        while (edges.back() * std::pow(2.0, 1.0 / 3) < rate / 2) edges.push_back(edges.back() * std::pow(2.0, 1.0 / 3));
        nbands = edges.size();
        for (size_t b = 0; b < NB; b++) {
            double fhz = (double)b * rate / (double)N;
            long long e = (long long)(std::upper_bound(edges.begin(), edges.end(), fhz) - edges.begin()) - 1;
            sband[b] = (size_t)std::clamp(e, 0LL, (long long)nbands - 1);
        }
        const size_t NSB = 1200;
        std::vector<double> sedges(NSB + 1);
        for (size_t e = 0; e < NSB; e++) sedges[e] = (double)e * (12.0 / (double)NSB) + -8.0;
        sedges[NSB] = 4.0;
        // per chunk of frames: counts [chunk][channel][band][bin]
        std::vector<uint32_t> sh(CHUNKS * C * nbands * NSB, 0u);
        parallel_for(F, CHUNKS, [&](size_t k, size_t t) {
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            frame_kar(k, &B);
            for (size_t c = 0; c < C; c++) {
                double g = apply_level ? corr[c][k] : 1.0;
                for (size_t b = 0; b < NB; b++) {
                    cplx I = predict(B, c, b) * g;
                    if (!(std::abs(I) * cap > std::abs(A[c][b]))) continue;
                    double v = std::log10(std::norm(A[c][b] - I) / (std::norm(I) + 1e-30) + 1e-30);
                    v = std::clamp(v, sedges[0], sedges[NSB] - 1e-9);
                    size_t e = (size_t)(std::upper_bound(sedges.begin(), sedges.end(), v) - sedges.begin()) - 1;
                    sh[((t * C + c) * nbands + sband[b]) * NSB + e]++;
                }
            }
        });
        rep.soft_q = (apply_level || !rep.drift_line || coded) ? 0.5 : 0.10;
        rho.assign(C, std::vector<double>(nbands, 1.0));
        std::vector<std::vector<char>> valid(C, std::vector<char>(nbands, 0));
        for (size_t c = 0; c < C; c++) {
            for (size_t bd = 0; bd < nbands; bd++) {
                std::vector<uint64_t> h(NSB, 0);
                uint64_t total = 0;
                for (size_t t = 0; t < CHUNKS; t++)
                    for (size_t e = 0; e < NSB; e++) h[e] += sh[((t * C + c) * nbands + bd) * NSB + e];
                for (uint64_t x : h) total += x;
                if (total < 50) continue;
                uint64_t cum = 0;
                size_t e = 0;
                for (; e < NSB; e++)
                    if ((double)(cum += h[e]) / (double)total >= rep.soft_q) break;
                rho[c][bd] = std::pow(10.0, sedges[std::min(e, NSB - 1) + 1]);
                valid[c][bd] = 1;
            }
            for (size_t bd = 1; bd < nbands; bd++)
                if (!valid[c][bd]) rho[c][bd] = rho[c][bd - 1], valid[c][bd] = valid[c][bd - 1];
        }
    }

    auto soft_cell = [&](const cplx& a, const cplx& I, size_t c, size_t b) {
        cplx v = a - I;
        double gain = 1 - strength * rho[c][sband[b]] * std::norm(I) / (std::norm(v) + 1e-30);
        return v * std::clamp(gain, 0.0, 1.0);
    };
    // Above a band-limited karaoke's cut the mix is kept in proportion to the vocal's share
    // of the half octave below it, frame by frame (the vocal's air where it sings, nothing
    // in its pauses): sum |V|^2 / sum |A|^2 there, each smoothed over frames
    std::vector<std::vector<double>> share;
    const bool use_share = opt.soft && top_b0 < NB;
    if (use_share) {
        const size_t lo = (size_t)((double)top_b0 / std::sqrt(2.0));
        std::vector<std::vector<double>> po(C, std::vector<double>(F, 0.0)), pm = po;
        parallel_for(F, CHUNKS, [&](size_t k, size_t) {
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            frame_kar(k, &B);
            for (size_t c = 0; c < C; c++) {
                double g = apply_level ? corr[c][k] : 1.0;
                for (size_t b = lo; b < top_b0; b++) {
                    po[c][k] += std::norm(soft_cell(A[c][b], predict(B, c, b) * g, c, b));
                    pm[c][k] += std::norm(A[c][b]);
                }
            }
        });
        share.assign(C, std::vector<double>(F));
        for (size_t c = 0; c < C; c++) {
            std::vector<double> so = gauss1d(po[c], 1.0), sm = gauss1d(pm[c], 1.0);
            for (size_t k = 0; k < F; k++) share[c][k] = std::clamp(so[k] / (sm[k] + 1e-30), 0.0, 1.0);
        }
    }

    // --- subtraction (the soft decision, or with --hard 3.0's rule), inverse STFT
    // (scipy.signal.istft)
    std::vector<double> thr(NB);
    for (size_t b = 0; b < NB; b++) thr[b] = opt.kvol * std::max(PI * ((double)b + 1.0) / (double)(N / 2), 0.15);
    size_t ext = (F - 1) * HOPV + N;
    Planar acc(C, std::vector<double>(ext, 0.0)), acc_i;
    if (instrumental) acc_i.assign(C, std::vector<double>(ext, 0.0));
    std::vector<double> norm(ext, 0.0);
    const size_t BLOCK = 256;
    std::vector<CVec> zs(BLOCK, CVec(N)), zs_i(instrumental ? BLOCK : 0, CVec(N));
    for (size_t k0 = 0; k0 < F; k0 += BLOCK) {
        size_t cnt = std::min(BLOCK, F - k0);
        parallel_for(cnt, CHUNKS, [&](size_t i, size_t) {
            size_t k = k0 + i;
            std::vector<CVec> A, B;
            st.frame(mix_in, k, 0.0, &A);
            frame_kar(k, &B);
            std::vector<CVec> V(C, CVec(NB)), Iv(instrumental ? C : 0, CVec(NB));
            for (size_t c = 0; c < C; c++) {
                double g = apply_level ? corr[c][k] : 1.0;
                for (size_t b = 0; b < NB; b++) {
                    cplx I = predict(B, c, b) * g;
                    if (instrumental) Iv[c][b] = I;
                    if (use_share && b >= top_b0) {
                        V[c][b] = A[c][b] * share[c][k];
                        continue;
                    }
                    if (opt.soft) {
                        V[c][b] = soft_cell(A[c][b], I, c, b);
                        continue;
                    }
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
            auto synth = [&](const std::vector<CVec>& S, CVec& z) {
                for (size_t b = 0; b < N; b++) {
                    cplx l = herm(S[0], b) * st.wsum;
                    cplx r = C > 1 ? herm(S[1], b) * st.wsum : cplx(0.0);
                    z[b] = l + cplx(0, 1) * r;
                }
                st.fft.inverse(z.data());
            };
            synth(V, zs[i]);
            if (instrumental) synth(Iv, zs_i[i]);
        });
        for (size_t i = 0; i < cnt; i++) {
            size_t k = k0 + i;
            const CVec& z = zs[i];
            for (size_t j = 0; j < N; j++) {
                size_t t = k * HOPV + j;
                double w = st.win[j];
                acc[0][t] += z[j].real() / (double)N * w;
                if (C > 1) acc[1][t] += z[j].imag() / (double)N * w;
                if (instrumental) {
                    acc_i[0][t] += zs_i[i][j].real() / (double)N * w;
                    if (C > 1) acc_i[1][t] += zs_i[i][j].imag() / (double)N * w;
                }
                norm[t] += w * w;
            }
        }
        if (!step(78 + (int)(20 * (k0 + cnt) / F))) return {};
    }
    auto finish_ola = [&](Planar& o) {  // the output, in place of the overlap-add buffer
        for (size_t c = 0; c < C; c++) {
            for (size_t t = 0; t < n; t++) {
                size_t e = t + N / 2;
                o[c][t] = o[c][e] / (norm[e] > 1e-10 ? norm[e] : 1.0);
            }
            o[c].resize(n);
            o[c].shrink_to_fit();
        }
    };
    finish_ola(acc);
    if (instrumental) {
        finish_ola(acc_i);
        *instrumental = std::move(acc_i);
    }
    if (report) *report = rep;
    step(100);
    return acc;
}

}  // namespace v4
}  // namespace utagoe
