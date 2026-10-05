"""Prototype separator: a whole-file version of Utagoe Rip's spectral subtraction
with the improvements under test as switches.

The decision rule is 3.0's ThVocalFFT (FFT 8192, hop 1024, Hann; zero a bin when the
instrumental explains it: |I|*cap > |O|, and in quality mode the phases agree within
thr[k]). What changes is how the instrumental estimate I is made and aligned:

    eq='scalar'     one gain for the whole file (what 3.0 does, per block)
    kill='scaled'   test the kill rule against the gain-scaled instrumental |g K|
                    instead of 3.0's raw |K| (only matters for eq='scalar')
    eq='perbin'     a complex gain per frequency bin, smoothed over `octave`: matches a
                    difference in EQ between the releases. passes/mag choose the
                    estimator (see _estimate_h)
    track=True      plus a per-frame level correction (compression differences),
                    estimated where the instrumental dominates
    track='auto'    the same, but only when it holds up out of sample: estimated on
                    the even bins, it must cut the residual on the odd bins by more
                    than `lvl_gain` (13 %, the middle of the gap on the dev songs:
                    at most 6 % without a dynamics difference, at least 19 % with
                    one); when the dynamics match, it mostly follows noise
    align='global'  one integer offset for the whole file (polarity detected)
    align='track'   an offset per analysis frame: coarse windows followed by a
                    per-frame phase-slope refinement; follows drift and wow
"""
from __future__ import annotations

import numpy as np
from scipy import ndimage, signal

N = 8192
HOP = 1024


def _stft(x):
    """(n, ch) -> (ch, bins, frames)"""
    return signal.stft(x.T, nperseg=N, noverlap=N - HOP, window='hann')[2]


def _istft(Z, n):
    """(ch, bins, frames) -> (n, ch)"""
    y = signal.istft(Z, nperseg=N, noverlap=N - HOP, window='hann')[1]
    out = np.zeros((n, y.shape[0]))
    m = min(n, y.shape[1])
    out[:m] = y[:, :m].T
    return out


# ---------------------------------------------------------------- alignment
MAX_LAG_SEC = 30.0   # the karaoke may start up to this much earlier or later


def find_lag(mix, kar, rate, max_sec=MAX_LAG_SEC, candidates=6, sep=256, return_all=False, excerpt_sec=60.0):
    """(L, sign): kar[t + L] ~ sign * mix's instrumental[t].

    GCC-PHAT on the mono sums gives candidate lags (the strongest |peaks|, at least
    `sep` apart); the sign of a peak is the polarity. The highest peak is not
    trusted: in loop-based music a repeat a few bars away can beat the true lag,
    especially once drift has smeared the true peak. As 3.0 does, every candidate is
    scored by a trial subtraction (below 1 kHz, where drift and wow barely matter,
    with a least-squares gain) and the one that removes the most wins. Songs longer
    than `excerpt_sec` are searched with an excerpt from the middle."""
    a, b = mix.mean(1), kar.mean(1)
    m = int(max_sec * rate)
    E = int(excerpt_sec * rate)
    if len(a) <= E:
        n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
        X = np.fft.rfft(b, n) * np.conj(np.fft.rfft(a, n))
        r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)
        mp, mn = min(m, len(b)), min(m, len(a))    # a short file covers fewer lags
        lags = np.concatenate([np.arange(0, mp), np.arange(-mn, 0)])
        vals = np.concatenate([r[:mp], r[n - mn:]])
    else:
        # a long song: correlate an excerpt from the middle against the karaoke +-m
        # around it (the FFT of a whole song would need gigabytes)
        a0 = len(a) // 2 - E // 2
        ae = a[a0:a0 + E]
        be = _segment(b, a0 - m, E + 2 * m)
        n = 1 << int(np.ceil(np.log2(len(ae) + len(be))))
        X = np.fft.rfft(be, n) * np.conj(np.fft.rfft(ae, n))
        r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)
        lags = np.arange(2 * m + 1) - m             # be[t + d] ~ ae[t]  =>  L = d - m
        vals = r[: 2 * m + 1]
    cands = []
    for j in np.argsort(np.abs(vals))[::-1]:
        if all(abs(lags[j] - c[0]) > sep for c in cands):
            cands.append((int(lags[j]), 1.0 if vals[j] >= 0 else -1.0))
            if len(cands) == candidates:
                break
    al, bl = _lowpass(a, rate), _lowpass(b, rate)
    return _rank(al, bl, cands, return_all)


def _rank(al, bl, cands, return_all=True):
    """Candidates sorted by a static trial subtraction (below 1 kHz, least-squares
    gain); each gets the sign of its gain."""
    scored = []
    for lag, sgn in cands:
        k = shift(np.pad(bl, (0, max(0, len(al) - len(bl))))[: len(al)][:, None], lag)[:, 0]
        res, g = _trial(al, k)
        scored.append((res, lag, 1.0 if g >= 0 else -1.0))
    scored.sort()
    if return_all:
        return [(lag, sgn) for _, lag, sgn in scored]
    return scored[0][1], scored[0][2]


def _window_peaks(a, b, rate, m, sep, win_sec=8.0, hop_sec=4.0, windows=16, per_window=2):
    """The `per_window` best GCC-PHAT lags (+-m) of up to `windows` 8 s windows spread
    evenly over the song."""
    W = int(win_sec * rate)
    if len(a) < W:
        return []
    count = min(windows, (len(a) - W) // int(hop_sec * rate) + 1)
    starts = np.linspace(0, len(a) - W, count).astype(np.int64) if count > 1 else np.array([0])
    out = []
    for w0 in starts:
        sa = a[w0:w0 + W]
        sb = _segment(b, int(w0) - m, W + 2 * m)
        n = 1 << int(np.ceil(np.log2(len(sa) + len(sb))))
        X = np.fft.rfft(sb, n) * np.conj(np.fft.rfft(sa, n))
        r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)[: 2 * m + 1]
        got = []
        for j in np.argsort(np.abs(r))[::-1]:
            lag = int(j) - m
            if all(abs(lag - g) > sep for g in got):
                got.append(lag)
                if len(got) == per_window:
                    break
        out += got
    return out


def band_hi(hz, rate):
    """A band edge in Hz, kept below Nyquist at low sample rates."""
    return min(hz, 0.45 * rate)


def _bandpass(x, rate):
    return signal.sosfilt(signal.butter(4, [100.0, band_hi(8000.0, rate)], 'band', fs=rate, output='sos'), x)


def _lowpass(x, rate, hz=1000.0):
    return signal.sosfilt(signal.butter(4, band_hi(hz, rate) / (rate / 2), output='sos'), x)


def _trial(a, k):
    """Trial subtraction a - g k with the least-squares g: (residual/original, g)."""
    kk = np.dot(k, k)
    if kk <= 0:
        return np.inf, 1.0
    g = np.dot(a, k) / kk
    return np.sum((a - g * k) ** 2) / max(np.dot(a, a), 1e-20), g


def shift(x, lag):
    """y[t] = x[t + lag] (zero outside)."""
    y = np.zeros_like(x)
    if lag >= 0:
        y[: len(x) - lag] = x[lag:]
    else:
        y[-lag:] = x[: len(x) + lag]
    return y


def _segment(x, start, n):
    """x[start:start+n] with zeros outside x."""
    out = np.zeros(n)
    lo, hi = max(start, 0), min(start + n, len(x))
    if hi > lo:
        out[lo - start: hi - start] = x[lo:hi]
    return out


def track_lags(mix, kar, rate, lag0, win_sec=2.0, hop_sec=1.0, search=64, search0=512, start=0, stop=None,
               seed=None):
    """Local lag of kar against mix (fractional samples) in overlapping windows.

    Tracking starts at the window nearest `seed` (default: the first) with lag0 and a
    wider range, then runs outwards in both directions, each window searching around
    the previous confident estimate, so a fast drift is followed. Integer part from
    GCC-PHAT (the largest |peak|, so either polarity), fraction from the slope of the
    cross-spectrum phase (100 Hz - 8 kHz, magnitude-weighted). Returns (centres, lags,
    weights); weight = |PHAT peak|.
    """
    a = mix.mean(1) if mix.ndim > 1 else mix
    b = kar.mean(1) if kar.ndim > 1 else kar
    W, H = int(win_sec * rate), int(hop_sec * rate)
    win = np.hanning(W)
    stop = len(a) if stop is None else stop
    n = 1 << int(np.ceil(np.log2(2 * W)))
    f = np.fft.rfftfreq(n, 1 / rate)
    band = (f > 100) & (f < band_hi(8000.0, rate))
    om = 2 * np.pi * f[band] / rate
    starts = list(range(start, max(start + 1, stop - W + 1), H))
    i0 = 0 if seed is None else int(np.argmin([abs(x + W / 2 - seed) for x in starts]))
    order = starts[i0:] + starts[:i0][::-1]          # forwards from the seed, then backwards
    centres, lags, weights = [], [], []
    centre, rng = lag0, search0
    seed_lag = lag0
    for s in order:
        if i0 > 0 and s == starts[i0 - 1]:           # turning back: restart at the seed
            centre, rng = seed_lag, search0
        sa = _segment(a, s, W) * win
        sb = _segment(b, s + centre, W) * win
        if np.sum(sa ** 2) < 1e-10 or np.sum(sb ** 2) < 1e-10:
            continue
        X = np.fft.rfft(sb, n) * np.conj(np.fft.rfft(sa, n))
        r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)
        cand = np.concatenate([np.arange(0, rng + 1), np.arange(-rng, 0)])
        vals = np.concatenate([r[: rng + 1], r[-rng:]])
        j = int(np.argmax(np.abs(vals)))          # either polarity
        d, peak = int(cand[j]), float(abs(vals[j]))
        pol = 1.0 if vals[j] >= 0 else -1.0
        # fraction: sb lags sa by D = d + frac, so the cross-spectrum phase is -omega*D
        # (plus pi for an inverted karaoke)
        Y = pol * X * np.exp(2j * np.pi * f * d / rate)
        ph, w = np.angle(Y[band]), np.abs(Y[band])
        frac = -np.sum(w * om * ph) / np.sum(w * om * om)
        lag = centre + d + frac
        centres.append(s + W / 2)
        lags.append(lag)
        weights.append(peak)
        # a confident window moves the search centre; a weak one doesn't
        if peak > 0.5 * np.median(weights) or len(weights) < 3:
            centre, rng = int(round(lag)), search
            if s == starts[i0]:
                seed_lag = centre                    # where the backward pass restarts
    o = np.argsort(centres)
    return np.array(centres)[o], np.array(lags)[o], np.array(weights)[o]


MIN_WEIGHT = 0.5    # windows below this fraction of the median peak are not fitted


def fit_lag_curve(centres, lags, weights, at, sigma_sec=1.5, rate=44100):
    """The lag at positions `at` (samples). Outliers (more than 2 samples from a
    running median) are dropped. A straight line (clock drift) is used when it fits
    to within 0.2 samples; otherwise a Gaussian-weighted local-linear fit with
    sigma_sec, which follows a wobble down to a few seconds' period.
    Returns (lags at `at`, True if the straight line was used)."""
    at = np.asarray(at, dtype=float)
    if len(lags) == 0:
        return np.zeros(len(at)), True
    if len(lags) < 3:
        return np.full(len(at), np.median(lags)), True
    med = ndimage.median_filter(lags, size=5, mode='nearest')
    keep = np.abs(lags - med) < 2.0
    # At an edge the running median sees mostly copies of the point itself and passes
    # anything, so the first and last two points are checked against the line through
    # their four inner neighbours instead; 8 samples, as the second pass (+-16) can
    # still correct anything closer
    m = len(lags)
    if m >= 6:
        for i, nb in ((0, slice(1, 5)), (1, slice(2, 6)), (m - 1, slice(m - 5, m - 1)), (m - 2, slice(m - 6, m - 2))):
            keep[i] = abs(np.polyval(np.polyfit(centres[nb], lags[nb], 1), centres[i]) - lags[i]) < 8.0
    # windows whose correlation peak is weak (the instrumental nearly silent, say in an
    # a cappella passage) are not fitted: the curve is carried over them from the
    # reliable ones around
    keep &= weights >= MIN_WEIGHT * np.median(weights)
    if keep.sum() < 3:
        keep[:] = True
    c, w, y = centres[keep], weights[keep], lags[keep]
    line = np.polyfit(c, y, 1, w=np.sqrt(w))
    if np.median(np.abs(np.polyval(line, c) - y)) < 0.2:
        return np.polyval(line, at), True
    s = sigma_sec * rate
    out = np.empty(len(at))
    for i0 in range(0, len(at), 4096):
        t = at[i0:i0 + 4096, None]
        k = w[None, :] * np.exp(-0.5 * ((c[None, :] - t) / s) ** 2) + 1e-30
        sw, sx, sy = k.sum(1), (k * c).sum(1), (k * y).sum(1)
        sxx, sxy = (k * c * c).sum(1), (k * c * y).sum(1)
        den = sw * sxx - sx * sx
        slope = np.where(np.abs(den) > 1e-12, (sw * sxy - sx * sy) / np.where(den == 0, 1, den), 0.0)
        out[i0:i0 + 4096] = (sy - slope * sx) / sw + slope * t[:, 0]
    return out, False


_KERNELS = {}


def _kernel_table(half, phases=1024, beta=9.0):
    """Kaiser-windowed sinc taps for `phases` fractional positions: (phases + 1, 2 * half)."""
    key = (half, phases, beta)
    if key not in _KERNELS:
        fr = np.arange(phases + 1)[:, None] / phases
        u = np.arange(-half + 1, half + 1)[None, :] - fr
        _KERNELS[key] = np.sinc(u) * np.i0(beta * np.sqrt(np.clip(1 - (u / half) ** 2, 0, None))) / np.i0(beta)
    return _KERNELS[key]


def frac_read(x, pos, half=32, phases=1024, beta=9.0):
    """x sampled at fractional positions `pos` (Kaiser-windowed sinc, 2*half taps; the
    fraction is quantised to 1/phases of a sample). Zero outside x."""
    tab = _kernel_table(half, phases, beta)
    i0 = np.floor(pos).astype(np.int64)
    q = np.rint((pos - i0) * phases).astype(np.int64)
    xp = np.pad(x, ((half, half + 1), (0, 0)))
    out = np.zeros((len(pos), x.shape[1]))
    for j, k in enumerate(range(-half + 1, half + 1)):
        idx = i0 + k + half
        ok = (idx >= 0) & (idx < len(xp))
        out[ok] += xp[idx[ok]] * tab[q[ok], j][:, None]
    return out


def _stft_along(x, frame_lag):
    """STFT of x (n, ch) with frame k taken at its own lag: the frame centred on
    k*HOP + frame_lag[k]. The integer part picks the samples, the fraction is a linear
    phase on the frame's spectrum, so nothing is resampled. Matches _stft's scaling
    and framing (centred frames, as scipy.signal.stft with boundary='zeros')."""
    n_frames = len(frame_lag)
    win = signal.get_window('hann', N)
    centres = np.arange(n_frames) * HOP
    li = np.floor(frame_lag).astype(np.int64)
    fr = frame_lag - li
    om = 2 * np.pi * np.arange(N // 2 + 1) / N
    out = np.empty((x.shape[1], N // 2 + 1, n_frames), dtype=complex)
    pad = N + int(np.max(np.abs(li))) + 1
    xp = np.pad(x, ((pad, pad), (0, 0)))
    for k0 in range(0, n_frames, 256):
        ks = np.arange(k0, min(n_frames, k0 + 256))
        starts = centres[ks] - N // 2 + li[ks] + pad
        idx = starts[:, None] + np.arange(N)[None, :]                    # (frames, N)
        for c in range(x.shape[1]):
            F = np.fft.rfft(xp[idx, c] * win, axis=1) / win.sum()
            out[c, :, k0:k0 + len(ks)] = (F * np.exp(1j * om[None, :] * fr[ks, None])).T  # advance by fr
    return out


def refine_frames(A, B, rate, f_hi, sigma_frames=3.0, prior=0.05):
    """Remaining lag per frame (samples) between mix frames A and karaoke frames B:
    the slope of the phase of A conj(B) over 100 Hz..f_hi, then a confidence-weighted
    Gaussian smoothing over frames. Bins are weighted by |B|^4 / (|A|^2 + |B|^2), so
    the ones the vocal dominates (whose phase follows the vocal) hardly count, and
    frames with little instrumental fall back towards 0 (no change)."""
    f = np.arange(A.shape[1]) * rate / N
    band = (f > 100) & (f < band_hi(f_hi, rate))
    om = 2 * np.pi * f[band] / rate
    a, b = A[:, band], B[:, band]
    X = np.sum(a * np.conj(b), axis=0)                                 # (bins, frames)
    pb, pa = np.sum(np.abs(b) ** 2, axis=0), np.sum(np.abs(a) ** 2, axis=0)
    w = pb ** 2 / (pa + pb + 1e-30)
    ph = np.angle(X)
    num = np.sum(w * om[:, None] * ph, axis=0)
    den = np.sum(w * om[:, None] ** 2, axis=0) + 1e-30
    delta = num / den                                                   # B lags A by delta
    conf = den / (np.median(den) + 1e-30)
    sm = lambda z: ndimage.gaussian_filter1d(z, sigma_frames, mode='nearest')
    return sm(conf * delta) / (sm(conf) + prior)


def pick_tracked(mix, kar, rate, top_windows=4, excerpt_sec=60.0, same=512, max_sec=MAX_LAG_SEC):
    """(lag, sign) for tracking.

    Candidates: the whole-file GCC-PHAT peaks (find_lag), plus the best few of the
    peaks of 8 s windows spread over the song: drift smears the whole-file peak and
    loop-based music adds rival peaks, but the true lag tends to win in some windows.
    Lags closer than `same` samples (the first tracking window's search range) count
    as one: drift moves the true peak around.

    Scoring: with drift, a trial subtraction at one fixed lag is unfair to the true
    lag, and below 1 kHz loops repeat so exactly that a repeat a few bars away can
    score as well. So every candidate is tracked through an excerpt (the middle
    `excerpt_sec`, or the whole song), and scored by a 100 Hz - 8 kHz trial
    subtraction along its own curve, where repeats differ. Tracking ignores polarity;
    the sign comes from the winner's trial gain. Returns (lag, sign, position): the
    winner's lag at `position`, the middle of the excerpt."""
    n = len(mix)
    a, b = mix.mean(1), kar.mean(1)
    m = int(max_sec * rate)
    cands = []
    for lag, sgn in find_lag(mix, kar, rate, return_all=True):
        if all(abs(lag - c[0]) > same for c in cands):
            cands.append((lag, sgn))
    # the windows look for drift and nearby repeats, within +-10 s
    extra = [(lag, 1.0) for lag in _window_peaks(a, b, rate, min(m, int(10.0 * rate)), 256)]
    extra = [c for c in _rank(_lowpass(a, rate), _lowpass(b, rate), extra)]
    added = 0
    for lag, sgn in extra:
        if added == top_windows:
            break
        if all(abs(lag - c[0]) > same for c in cands):
            cands.append((lag, sgn))
            added += 1
    E = min(n, int(excerpt_sec * rate))
    e0 = (n - E) // 2
    ab = _bandpass(a[e0:e0 + E], rate)
    t = np.arange(e0, e0 + E, dtype=float)
    sub = t[::256]
    mid = e0 + E // 2
    scored = []
    for c, _ in cands:
        ce, lags, w = track_lags(a, b, rate, c, start=e0, stop=e0 + E, seed=mid)
        curve, _ = fit_lag_curve(ce, lags, w, sub, rate=rate)
        k = frac_read(b[:, None], t + np.interp(t, sub, curve), half=16)[:, 0]
        res, g = _trial(ab, _bandpass(k, rate))
        scored.append((res, g, np.interp(mid, sub, curve)))
    # The best trial wins; candidates within 5 % of it (a song that repeats itself, say)
    # are told apart by how much of the two files overlaps at their lag
    best_res = min(r for r, _, _ in scored)
    overlap = lambda lag: max(0.0, min(n, len(b) - lag) - max(0.0, -lag))
    win, win_ov = 0, -1.0
    for j, (r, _, lag) in enumerate(scored):
        if r <= best_res * 1.05 and overlap(lag) > win_ov:
            win, win_ov = j, overlap(lag)
    r, g, lag = scored[win]
    return int(round(lag)), 1.0 if g >= 0 else -1.0, mid


def align_frames(mix, kar, A, rate, lag0, seed=None, keep_line=0.75, cap=1.2):
    """The lag of kar against mix for every STFT frame: two passes of coarse windows,
    then, unless they fit a straight line (offset plus clock drift) that the first
    per-frame refinement confirms, two per-frame refinements (low band first,
    unambiguous to ~11 samples; then up to 6 kHz for precision)."""
    n_frames = A.shape[2]
    centres = np.arange(n_frames) * HOP
    t = np.arange(len(mix), dtype=float)
    lag, is_line = fit_lag_curve(*track_lags(mix, kar, rate, lag0, seed=seed), centres, rate=rate)
    # second coarse pass on the karaoke read along the first curve: what is left is
    # nearly constant, so a fast drift no longer smears each window's estimate
    # The second pass measures accurately but the first curve can carry a sawtooth, so
    # the final curve is fitted to the second pass's absolute lags, not added on top
    k1 = frac_read(kar, t + np.interp(t, centres, lag))
    c2, r2, w2 = track_lags(mix, k1, rate, 0, search=16, search0=16)
    lag, is_line = fit_lag_curve(c2, np.interp(c2, centres, lag) + r2, w2, centres, rate=rate)
    # A wobble faster than the windows (a 33 rpm record: 1.8 s) averages out in them and
    # can pass for a straight line, so the first per-frame refinement always runs; the
    # line is kept when that finds nothing (median correction under `keep_line`)
    # An EQ difference has a phase response (a low shelf delays the low band) that the
    # refinement would read as timing, so it compares the mix with the karaoke through
    # the EQ's phase (_eq_phase), re-estimated once the first pass has taken out most of
    # a wobble (a wobble smears the estimate).
    d1 = refine_frames(A, _eq_phase(A, _stft_along(kar, lag), rate, cap) * _stft_along(kar, lag), rate, 2000.0)
    inner = d1[40:-40] if len(d1) > 100 else d1
    if is_line and np.median(np.abs(inner)) < keep_line:
        return lag
    lag = lag + d1
    B = _stft_along(kar, lag)
    return lag + refine_frames(A, _eq_phase(A, B, rate, cap) * B, rate, 6000.0)


def _eq_phase(A, B, rate, cap):
    """The phase of a first per-band EQ estimate (one pass, least squares, 1/3 octave)
    without its linear part: the linear part is a pure delay, which is timing and has to
    stay visible to the refinement. Unit magnitude, (ch, bins, 1)."""
    H = _estimate_h(A, B, 1.0, cap, 1 / 3, 1, 'regress')
    f = np.arange(H.shape[1]) * rate / N
    band = (f > 100) & (f < band_hi(6000.0, rate))
    om = 2 * np.pi * f[band] / rate
    w = np.sum(np.abs(B[:, band]) ** 2, axis=2)
    tau = -np.sum(w * om * np.angle(H[:, band]), axis=1) / np.sum(w * om * om, axis=1)
    H = H * np.exp(1j * 2 * np.pi * np.arange(H.shape[1])[None, :] / N * tau[:, None])
    return np.exp(1j * np.angle(H))[:, :, None]


# ---------------------------------------------------------------- helpers
def _smooth_bins(z, octave):
    """Sum over a band of +-octave/2 around every bin (at least +-2 bins), along the last
    axis. octave=None: the sum over all bins (one gain for the whole spectrum)."""
    if octave is None:
        return np.broadcast_to(np.sum(z, axis=-1, keepdims=True), z.shape).copy()
    nb = z.shape[-1]
    k = np.arange(nb)
    lo = np.maximum(0, np.minimum(k - 2, np.floor(k * 2 ** (-octave / 2)))).astype(int)
    hi = np.minimum(nb - 1, np.maximum(k + 2, np.ceil(k * 2 ** (octave / 2)))).astype(int)
    c = np.concatenate([np.zeros(z.shape[:-1] + (1,), z.dtype), np.cumsum(z, axis=-1)], axis=-1)
    return c[..., hi + 1] - c[..., lo]


def _phase_diff(a, b):
    d = np.abs(np.angle(a) - np.angle(b))
    return np.where(d > np.pi, 2 * np.pi - d, d)


def _thr(kvol):
    half = N // 2
    ramp = np.pi * (np.arange(half + 1) + 1.0) / half
    return kvol * np.maximum(ramp, 0.15)


def _estimate_h(A, B, g, cap, octave, passes, mag):
    """Complex gain per bin, (ch, bins).

    passes=1, mag='regress': least squares sum A conj(B) / sum |B|^2 over all cells
    (unbiased while the karaoke itself carries no noise).
    passes=2: re-estimate from the cells the first estimate says the instrumental
    dominates. That selection truncates A, which biases least squares low; mag='power'
    takes |H| from the power ratio of those cells instead (and keeps the phase)."""
    H = np.full(A.shape[:2], g, dtype=complex)
    w = np.ones(A.shape, dtype=bool)
    for p in range(passes):
        if p:
            w = np.abs(H[:, :, None] * B) * cap > np.abs(A)
        num = _smooth_bins(np.sum(np.where(w, A * np.conj(B), 0), axis=2), octave)
        den = _smooth_bins(np.sum(np.where(w, np.abs(B) ** 2, 0), axis=2), octave) + 1e-20
        H = num / den
        if mag == 'power' and p:
            pa = _smooth_bins(np.sum(np.where(w, np.abs(A) ** 2, 0), axis=2), octave)
            H = np.sqrt(pa / den) * np.exp(1j * np.angle(H))
    return H


def _choose_eq(A, B, g, cap, passes, mag, models=(1 / 3, 1.0, None)):
    """Which EQ model predicts the instrumental best out of sample: each is estimated
    on the even frames and judged by the residual |A - H B|^2 on the odd ones. Per-band
    detail only pays where the releases' EQ really differs; elsewhere its estimation
    noise costs more than it gains."""
    ev, od = slice(0, None, 2), slice(1, None, 2)
    best, best_r = models[0], np.inf
    for m in models:
        H = _estimate_h(A[:, :, ev], B[:, :, ev], g, cap, m, passes, mag)
        r = float(np.sum(np.abs(A[:, :, od] - H[:, :, None] * B[:, :, od]) ** 2))
        if r < best_r:
            best, best_r = m, r
    return best


def _level_track(A, I, cap, sigma_frames=2.0, bins=slice(None)):
    """Per-frame gain correction for I (compression differences), in three steps, each
    smoothed over frames: least squares over all cells (the vocal is uncorrelated with
    the instrumental, so this is unbiased, if noisy, even where I is far off, as in the
    vocal's pauses when the album's compressor works less there; selecting cells by the
    uncorrected I would find none there), least squares over the cells the corrected I
    says the instrumental dominates, and |gain| from the power ratio of those cells
    (the selection truncates |A|, which biases least squares low). Clipped to +-12 dB."""
    A, I = A[:, bins], I[:, bins]
    sm = lambda z: ndimage.gaussian_filter1d(z, sigma_frames, axis=1, mode='nearest')

    def ls(w, X):
        num = sm(np.sum(np.where(w, np.real(A * np.conj(X)), 0), axis=1))      # (ch, frames)
        den = sm(np.sum(np.where(w, np.abs(X) ** 2, 0), axis=1))
        return np.clip(np.where(den > 1e-12, num / (den + 1e-20), 1.0), 0.25, 4.0)
    c0 = ls(True, I)
    I0 = I * c0[:, None, :]
    c1 = ls(np.abs(I0) * cap > np.abs(A), I0)
    I1 = I0 * c1[:, None, :]
    w = np.abs(I1) * cap > np.abs(A)
    pa = sm(np.sum(np.where(w, np.abs(A) ** 2, 0), axis=1))
    pi = sm(np.sum(np.where(w, np.abs(I1) ** 2, 0), axis=1))
    r = np.where(pi > 1e-12, np.sqrt(pa / (pi + 1e-20)), 1.0)
    return np.clip(c0 * np.clip(c1 * r, 0.25, 4.0), 0.25, 4.0)


def _level_gain(A, I, cap, c):
    """How much the correction c cuts the residual |A - c I|^2 on the odd bins, in the
    cells the instrumental dominates (1 - after / before)."""
    a, i = A[:, 1::2], I[:, 1::2]
    w = np.abs(i) * cap > np.abs(a)
    e = slice(N // HOP // 2, -(N // HOP // 2))     # not the frames reaching past an end
    pa = np.sum(np.where(w, np.abs(a) ** 2, 0), axis=1)[:, e]          # (ch, frames)
    x = np.sum(np.where(w, np.real(a * np.conj(i)), 0), axis=1)[:, e]
    ii = np.sum(np.where(w, np.abs(i) ** 2, 0), axis=1)[:, e]
    c = c[:, e]
    r0 = np.sum(pa - 2 * x + ii)
    r1 = np.sum(pa - 2 * c * x + c * c * ii)
    return 1 - r1 / r0 if r0 > 1e-30 else 0.0      # nothing to correct in silence


# ---------------------------------------------------------------- the separator
# Frames are cut from the karaoke at one lag each, the fraction applied as a linear
# phase: exact while the lag holds still over a frame. When it moves by more than this
# across one (N samples; 0.6 is about 70 ppm of drift) the karaoke is resampled along
# the lag curve instead, which costs some accuracy near Nyquist (a finite sinc).
MAX_STRETCH = 0.6


def stretch(frame_lag):
    """Typical change of the lag across one frame (N = 8 hops), in samples."""
    d = N // HOP
    return float(np.median(np.abs(frame_lag[d:] - frame_lag[:-d]))) if len(frame_lag) > d else 0.0


def separate(mix, kar, rate=44100, eq='scalar', kill='raw', track=False, kvol=1.2, quality=True,
             octave=1 / 3, passes=2, mag='power', align='global', lvl_gain=0.13):
    """Returns the vocal estimate (same shape as mix)."""
    n = len(mix)
    if n == 0 or len(kar) == 0:                                   # nothing to align
        return mix.copy()
    kar = kar[:n + int(MAX_LAG_SEC * rate) + N]                  # beyond the lag search
    seed = None
    if align == 'track':
        lag0, sign, seed = pick_tracked(mix, kar, rate)
    else:
        lag0, sign = find_lag(mix, kar, rate)
    kar = kar * sign                                             # inverted karaoke
    A = _stft(mix)                                               # (ch, bins, frames)
    if align == 'global':
        k = shift(np.pad(kar, ((0, max(0, n - len(kar))), (0, 0)))[:n], lag0)
        B = _stft(k)
    elif align == 'track':
        frame_lag = align_frames(mix, kar, A, rate, lag0, seed, cap=min(kvol, 1.5) if quality else kvol)
        pos = np.arange(n) + np.interp(np.arange(n), np.arange(len(frame_lag)) * HOP, frame_lag)
        if stretch(frame_lag) > MAX_STRETCH:
            # the lag moves within a frame (fast drift, wow), so a frame cut at one lag is
            # off towards its edges: resample the karaoke along the curve instead
            k = frac_read(kar, pos, half=64, beta=10.0)
            B = _stft(k)
        else:
            B = _stft_along(kar, frame_lag)
            k = frac_read(kar, pos)
    else:
        raise ValueError(align)
    cap = min(kvol, 1.5) if quality else kvol
    g = np.sum(mix * k) / max(np.sum(k * k), 1e-20)

    if eq == 'scalar':
        I = min(g, cap) * B
        mag_i = np.abs(I) if kill == 'scaled' else np.abs(B)      # 3.0 tests the raw instrumental
    elif eq == 'perbin':
        oct_ = _choose_eq(A, B, g, cap, passes, mag) if octave == 'cv' else octave
        I = _estimate_h(A, B, g, cap, oct_, passes, mag)[:, :, None] * B
        mag_i = np.abs(I)
    else:
        raise ValueError(eq)

    if track:
        c = _level_track(A, I, cap)
        if track != 'auto' or _level_gain(A, I, cap, _level_track(A, I, cap, bins=slice(0, None, 2))) > lvl_gain:
            I = I * c[:, None, :]
            mag_i = np.abs(I)

    V = A - I
    kill_mask = mag_i * cap > np.abs(A)
    if quality:
        kill_mask &= _phase_diff(A, I) < _thr(kvol)[None, :, None]
    V[kill_mask] = 0
    return _istft(V, n)


ENGINES = {
    # 3.0's approach on the whole file: checks the prototype against 3.0
    'proto-scalar': dict(eq='scalar'),
    # 3.0 plus one change: kill rule against the gain-scaled instrumental
    'proto-scalar-kill': dict(eq='scalar', kill='scaled'),
    # per-band EQ matching: one-pass least squares, and the two-pass power estimator
    'proto-eq-ls1': dict(eq='perbin', passes=1, mag='regress'),
    'proto-eq': dict(eq='perbin'),
    # + frame-by-frame alignment (drift, wow)
    'proto-eq-align': dict(eq='perbin', align='track'),
    # + per-frame level tracking (compression), always or only when it varies enough
    'proto-eq-align-lvl': dict(eq='perbin', align='track', track=True),
    'proto-eq-align-auto': dict(eq='perbin', align='track', track='auto'),
    # the configuration chosen on the dev set (identical to proto-eq-align-auto)
    'v4': dict(eq='perbin', align='track', track='auto'),
    # tried: the EQ model chosen per song out of sample (1/3 octave, 1 octave or one
    # gain). No difference on the dev set, so not part of v4
    'v4-cv': dict(eq='perbin', align='track', track='auto', octave='cv'),
}
