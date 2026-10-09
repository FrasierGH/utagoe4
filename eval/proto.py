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


def fit_lag_curve(centres, lags, weights, at, sigma_sec=1.5, rate=44100, const_lag=0.0):
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
        span = c.max() - c.min()
        sw = np.sum(w)
        cm = np.sum(w * c) / sw
        s_r = np.sqrt(np.sum(w * (np.polyval(line, c) - y) ** 2) / sw)
        s_c = np.sqrt(np.sum(w * (c - cm) ** 2) / sw)
        se_drift = s_r / (s_c * np.sqrt(max(len(c) - 2, 1)) + 1e-30) * span
        if const_lag and abs(line[0] * span) < max(const_lag, 3 * se_drift):
            # no drift to speak of (identical clocks): a constant, without the slope's noise.
            # Under 3 standard errors also counts: an EQ difference biases each window's
            # fraction by an amount that follows the music, which can fake a small drift
            return np.full(len(at), float(np.sum(w * y) / np.sum(w))), True
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


def align_frames(mix, kar, A, rate, lag0, seed=None, keep_line=0.75, cap=1.2, const_lag=0.0, wow=False):
    """The lag of kar against mix for every STFT frame: two passes of coarse windows,
    then, unless they fit a straight line (offset plus clock drift) that the first
    per-frame refinement confirms, two per-frame refinements (low band first,
    unambiguous to ~11 samples; then up to 6 kHz for precision)."""
    n_frames = A.shape[2]
    centres = np.arange(n_frames) * HOP
    t = np.arange(len(mix), dtype=float)
    lag, is_line = fit_lag_curve(*track_lags(mix, kar, rate, lag0, seed=seed), centres, rate=rate, const_lag=const_lag)
    # second coarse pass on the karaoke read along the first curve: what is left is
    # nearly constant, so a fast drift no longer smears each window's estimate
    # The second pass measures accurately but the first curve can carry a sawtooth, so
    # the final curve is fitted to the second pass's absolute lags, not added on top
    k1 = frac_read(kar, t + np.interp(t, centres, lag))
    c2, r2, w2 = track_lags(mix, k1, rate, 0, search=16, search0=16)
    lag, is_line = fit_lag_curve(c2, np.interp(c2, centres, lag) + r2, w2, centres, rate=rate, const_lag=const_lag)
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
        return lag, True
    if wow:
        # A wobble the coarse windows missed can leave errors past the 2 kHz band's
        # unambiguous range (about 11 samples); a start below 500 Hz (about 44 samples)
        # reaches them, but where there is nothing to reach it only adds noise. So both
        # paths run to the end, and the low-band one is kept where the instrumental fits
        # clearly better along it (judged below).
        B = _stft_along(kar, lag)
        d0 = refine_frames(A, _eq_phase(A, B, rate, cap) * B, rate, 500.0)

        def finish(l):
            Bx = _stft_along(kar, l)
            l = l + refine_frames(A, _eq_phase(A, Bx, rate, cap) * Bx, rate, 2000.0)
            Bx = _stft_along(kar, l)
            return l + refine_frames(A, _eq_phase(A, Bx, rate, cap) * Bx, rate, 6000.0)
        la, lb = finish(lag), finish(lag + d0)
        # judged on the cells the instrumental dominates along the usual path (the vocal
        # stays out of it), each path with its own EQ: the low-band path must leave under
        # 0.9 of the residual
        Ba, Bb = _stft_along(kar, la), _stft_along(kar, lb)
        Ia = _estimate_h(A, Ba, 1.0, cap, 1 / 3, 1, 'regress')[:, :, None] * Ba
        Ib = _estimate_h(A, Bb, 1.0, cap, 1 / 3, 1, 'regress')[:, :, None] * Bb
        dom = np.abs(Ia) * cap > np.abs(A)
        ra = np.sum(np.where(dom, np.abs(A - Ia) ** 2, 0))
        rb = np.sum(np.where(dom, np.abs(A - Ib) ** 2, 0))
        return (lb if rb < 0.9 * ra else la), False
    lag = lag + d1
    B = _stft_along(kar, lag)
    return lag + refine_frames(A, _eq_phase(A, B, rate, cap) * B, rate, 6000.0), False


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


def _lowpass_cut(A, B, rate):
    """A band-limited karaoke's cut (the first bin of its top band without reference), or
    None. Bins where the karaoke's mean power is 20 dB under its usual share of the
    mix's (1-8 kHz), those where the mix itself is silent (-50 dB) counting either way,
    median of 9 bins; the top run of them, gaps under 500 Hz bridged, starting below
    20 kHz, 80% such bins, with at least 1 kHz of them where the mix is not silent, and
    the karaoke's own spectrum 20 dB lower just above the run's start than just below."""
    pa = np.mean(np.abs(A) ** 2, axis=(0, 2))
    pb = np.mean(np.abs(B) ** 2, axis=(0, 2))
    f = np.arange(len(pa)) * rate / N
    mid = (f > 1000) & (f < 8000)
    r = pb / (pa + 1e-30)
    low = r < 1e-2 * np.median(r[mid])
    dead = pa < 1e-5 * np.median(pa[mid])
    nr = ndimage.median_filter(low | dead, 9)
    gap = int(np.ceil(500.0 * N / rate))
    b, b0 = len(nr) - 1, len(nr)          # b0: the lowest bin of the run so far
    while b >= 0:
        if nr[b]:
            b0 = b
        elif b0 - b > gap:
            break
        b -= 1
    if not (b0 < len(nr) and f[b0] < 20000 and np.mean(nr[b0:]) >= 0.8 and
            np.sum(low[b0:] & ~dead[b0:]) * rate / N >= 1000):
        return None
    # and the karaoke's own spectrum falls off a cliff there (20 dB from the 750 Hz below
    # the cut to the 750 Hz above it, 250 Hz either side left out): a dark arrangement
    # under a bright vocal also leaves the karaoke far under the mix up there, but its
    # roll-off is gradual
    lo = (f >= f[b0] - 1000) & (f < f[b0] - 250)
    hi = (f >= f[b0] + 250) & (f < f[b0] + 1000)
    if not (lo.any() and hi.any() and np.mean(pb[lo]) > 100 * np.mean(pb[hi])):
        return None
    return b0


def _lowpass_share(out, A, B, rate, ref=None):
    """A band-limited karaoke (a rip with a low-pass the original lacks): above its cut the
    karaoke has nothing, so nothing there can be subtracted. There the mix is kept in
    proportion to the vocal's share of the half octave below the cut, frame by frame (the
    vocal's air where it sings, nothing in its pauses)."""
    b0 = _lowpass_cut(A, B, rate)
    if b0 is None:
        return out
    below = slice(int(b0 / 2 ** 0.5), b0)
    po = np.sum(np.abs((out if ref is None else ref)[:, below]) ** 2, axis=1)   # (ref: the share's source)
    pm = np.sum(np.abs(A[:, below]) ** 2, axis=1)
    share = np.clip(ndimage.gaussian_filter1d(po, 1.0, axis=1, mode='nearest') /
                    (ndimage.gaussian_filter1d(pm, 1.0, axis=1, mode='nearest') + 1e-30), 0, 1)
    out = out.copy()
    out[:, b0:, :] = A[:, b0:, :] * share[:, None, :]
    return out


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


# per-bin medians of |A - H B| come from a histogram of log10 |r| (as the C++ engine
# streams them): 0.02 decade bins
HUBER_BINS = np.linspace(-12.0, 2.0, 701)


def _hist_median(r):
    """Median along the last axis from the HUBER_BINS histogram (upper bin edge)."""
    lr = np.clip(np.log10(r + 1e-300), HUBER_BINS[0], HUBER_BINS[-1] - 1e-9)
    idx = np.searchsorted(HUBER_BINS, lr, side='right') - 1                # (ch, bins, frames)
    nh = len(HUBER_BINS) - 1
    flat = idx + nh * np.arange(idx.shape[0] * idx.shape[1]).reshape(idx.shape[0], idx.shape[1], 1)
    h = np.bincount(flat.ravel(), minlength=idx.shape[0] * idx.shape[1] * nh).reshape(idx.shape[0], idx.shape[1], nh)
    cdf = np.cumsum(h, axis=2)
    k = np.argmax(cdf >= (r.shape[2] + 1) // 2, axis=2)                     # the first bin holding the median
    return 10 ** HUBER_BINS[k + 1]


def _estimate_h_huber(A, B, octave, iters=4, k=1.5):
    """Complex gain per bin by iteratively reweighted least squares with Huber weights:
    cells whose residual |A - H B| is well above the bin's median (the vocal) count less,
    so the estimate is neither pulled by the vocal nor truncated by a cell selection."""
    H = _smooth_bins(np.sum(A * np.conj(B), axis=2), octave) / (_smooth_bins(np.sum(np.abs(B) ** 2, axis=2), octave) + 1e-20)
    for _ in range(iters):
        r = np.abs(A - H[:, :, None] * B)
        delta = k * _hist_median(r)[:, :, None]
        w = np.minimum(1.0, delta / (r + 1e-30))
        H = _smooth_bins(np.sum(w * A * np.conj(B), axis=2), octave) / (_smooth_bins(np.sum(w * np.abs(B) ** 2, axis=2), octave) + 1e-20)
    return H


def _estimate_h_mimo(A, B, octave, iters=2, k=1.5, ridge=1e-6):
    """A 2x2 complex EQ per bin (each output channel from both karaoke channels), Huber
    weighted as _estimate_h_huber. Returns (ch_out, ch_in, bins)."""
    C, NBn, T = A.shape
    H = np.zeros((C, C, NBn), dtype=complex)
    for c in range(C):
        w = np.ones((NBn, T))
        for it in range(iters + 1):
            R = np.zeros((NBn, C, C), dtype=complex)
            pv = np.zeros((NBn, C), dtype=complex)
            for d in range(C):
                pv[:, d] = _smooth_bins(np.sum(w * A[c] * np.conj(B[d]), axis=1)[None, :], octave)[0]
                for e in range(C):
                    R[:, d, e] = _smooth_bins(np.sum(w * B[d] * np.conj(B[e]), axis=1)[None, :], octave)[0]
            tr = np.real(np.trace(R, axis1=1, axis2=2))[:, None, None]
            R = R + ridge * tr * np.eye(C)[None]
            # A_c ~ sum_d h_d B_d: h R^T = p (R[d, e] = sum B_d conj(B_e))
            Rt = np.transpose(R, (0, 2, 1))
            sing = np.abs(np.linalg.det(Rt)) == 0             # a band silent in both channels
            Rt[sing] = np.eye(C)
            h = np.linalg.solve(Rt, pv[:, :, None])[:, :, 0]
            h[sing] = 0
            H[c] = h.T
            if it == iters:
                break
            r = np.abs(A[c] - np.einsum('db,dbt->bt', H[c], B))
            delta = k * _hist_median(r[None])[0][:, None]
            w = np.minimum(1.0, delta / (r + 1e-30))
    return H


# Soft decision: bands of 1/3 octave (below 50 Hz one band) in which the model error is
# measured, and the histogram of log10 |A - I|^2 / |I|^2 it is read from
SOFT_BINS = np.linspace(-8.0, 4.0, 1201)


def _soft_bands(nb, rate):
    f = np.arange(nb) * rate / N
    edges = [0.0, 50.0]
    while edges[-1] * 2 ** (1 / 3) < rate / 2:
        edges.append(edges[-1] * 2 ** (1 / 3))
    band = np.clip(np.searchsorted(edges, f, side='right') - 1, 0, len(edges) - 1)
    return band, len(edges)


def lossy(X, rate, lo=11000.0, hi=20500.0, step=250.0, width=1000.0, drop_db=20.0):
    """A lossy coder's low-pass: somewhere between `lo` and `hi`, the mean spectrum (all
    channels and frames) drops by more than `drop_db` within `width`. Natural roll-offs
    and anti-alias filters near Nyquist are gentler or higher."""
    p = np.mean(np.abs(X) ** 2, axis=(0, 2))
    f = np.arange(len(p)) * rate / N
    edges = np.arange(lo, min(hi, rate / 2) + 1e-9, step)
    lvl = np.array([10 * np.log10(np.mean(p[(f >= e) & (f < e + step)]) + 1e-30) for e in edges])
    k = int(round(width / step))
    return len(lvl) > k and float(np.max(lvl[:-k] - lvl[k:])) > drop_db


def _soft_gain(A, I, cap, q, strength, rate, smooth=None):
    """max(0, 1 - strength rho |I|^2 / |A - I|^2) per cell: rho is the model error, the
    q-quantile of |A - I|^2 / |I|^2 over the cells the instrumental dominates, per band
    (one value per band). An exact model (rho -> 0) subtracts and keeps
    everything; a poor one suppresses cells where the residual is mostly model error.

    smooth=(ff, tt, pause) (4.4): in frames that look like the vocal's pauses (the
    frame's residual under `pause` times the model error it expects), the expected model
    error strength rho |I|^2 and the residual |A - I|^2 are each averaged over ff bins x
    tt frames before their ratio is taken, so an isolated cell of leftover instrumental
    is judged with its quieter neighbours. Frames where the vocal sings keep each cell's
    own decision."""
    pi, pv = np.abs(I) ** 2, np.abs(A - I) ** 2
    dom = np.abs(I) * cap > np.abs(A)
    band, nbands = _soft_bands(A.shape[1], rate)
    lr = np.log10(pv / (pi + 1e-30) + 1e-30)
    rho = np.ones((A.shape[0], nbands))
    valid = np.zeros((A.shape[0], nbands), dtype=bool)
    for c in range(A.shape[0]):
        for b in range(nbands):
            sel = dom[c][band == b]
            v = lr[c][band == b][sel]
            if v.size < 50:
                continue
            h, _ = np.histogram(np.clip(v, SOFT_BINS[0], SOFT_BINS[-1] - 1e-9), SOFT_BINS)
            cdf = np.cumsum(h) / h.sum()
            i = int(np.searchsorted(cdf, q))
            rho[c, b] = 10 ** SOFT_BINS[i + 1]
            valid[c, b] = True
    # per bin: the band's value (bands with too few cells take the next band down's)
    for b in range(1, nbands):
        rho[:, b] = np.where(valid[:, b], rho[:, b], rho[:, b - 1])
        valid[:, b] |= valid[:, b - 1]
    rb = rho[:, band][:, :, None]
    e = strength * rb * pi
    g = np.clip(1 - e / (pv + 1e-30), 0.0, 1.0)
    if smooth:
        ff, tt, pause = smooth
        se = ndimage.uniform_filter(e, size=(1, ff, tt), mode='nearest')
        sv = ndimage.uniform_filter(pv, size=(1, ff, tt), mode='nearest')
        gs = np.clip(1 - se / (sv + 1e-30), 0.0, 1.0)
        ratio = np.sum(pv, axis=(0, 1)) / (np.sum(e, axis=(0, 1)) / strength + 1e-30)
        g = np.where((ratio < pause)[None, None, :], gs, g)
    return g


def _estimate_h(A, B, g, cap, octave, passes, mag, iters=4):
    """Complex gain per bin, (ch, bins).

    passes=1, mag='regress': least squares sum A conj(B) / sum |B|^2 over all cells
    (unbiased while the karaoke itself carries no noise).
    passes=2: re-estimate from the cells the first estimate says the instrumental
    dominates. That selection truncates A, which biases least squares low; mag='power'
    takes |H| from the power ratio of those cells instead (and keeps the phase)."""
    if mag == 'huber':
        return _estimate_h_huber(A, B, octave, iters)
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


def _eq_shaped(H, rate, f_hi=10000.0, db_limit=1.5):
    """An EQ difference with shape (4.4): the per-channel EQ, averaged over half an octave
    around each bin, more than `db_limit` off its median somewhere between 100 Hz and
    f_hi (10 kHz, or a quarter octave below a band-limited karaoke's cut, where the
    missing band would look like shape). Averaging leaves broad curves (shelves, wide
    peaks: a mastering EQ) and removes narrow blips, which an estimate can show where the
    vocal and the accompaniment share notes."""
    f = np.arange(H.shape[1]) * rate / N
    sel = (f > 100) & (f < f_hi)
    db = 20 * np.log10(np.abs(H) + 1e-12)
    dev = db - np.median(db[:, sel], axis=1, keepdims=True)
    sm = np.real(_smooth_bins(dev, 1 / 2) / _smooth_bins(np.ones_like(dev), 1 / 2))
    return float(np.max(np.abs(sm[:, sel]))) > db_limit


def _level_gain(A, I, cap, c, even=False):
    """How much the correction c cuts the residual |A - c I|^2 on the odd bins, in the
    cells the instrumental dominates (1 - after / before). even=True (4.4): dominated in
    the even bin just below, the correction's own bins. Chosen by the odd bin itself, a
    loud vocal that partly cancels the instrumental there makes the cell look dominated,
    the same bias the correction's own cell selection has, so a correction that only
    follows that bias passes the test."""
    a, i = A[:, 1::2], I[:, 1::2]
    if even:
        ae, ie = A[:, 0:-1:2][:, :a.shape[1]], I[:, 0:-1:2][:, :a.shape[1]]
        w = np.abs(ie) * cap > np.abs(ae)
    else:
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
             octave=1 / 3, passes=2, mag='power', align='global', lvl_gain=0.13, decision='hard', soft_q=0.5,
             huber_iters=4, soft_q_lo=0.10, const_lag=0.0, mimo=False, eq_q=None, noref=False, wow=False,
             level_even=False, eq_fine=None, smooth=None):
    """Returns the vocal estimate (same shape as mix)."""
    n = len(mix)
    if n == 0 or len(kar) == 0 or not np.any(kar):                # nothing to align
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
        frame_lag, is_line = align_frames(mix, kar, A, rate, lag0, seed, cap=min(kvol, 1.5) if quality else kvol,
                                          const_lag=const_lag, wow=wow)
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
        Hd = _estimate_h(A, B, g, cap, oct_, passes, mag, huber_iters)
        I = Hd[:, :, None] * B
        mimo_used = False
        if mimo and A.shape[0] == 2:
            # a karaoke whose stereo image differs (another width or balance): a 2x2 EQ,
            # each output channel from both karaoke channels, where it predicts held-out
            # frames clearly better (fitted on the even frames, judged on the odd ones' cells
            # the instrumental dominates; 20% less residual). With the image the same its
            # extra freedom only adds estimation noise.
            ev, od = slice(0, None, 2), slice(1, None, 2)
            Hd_e = _estimate_h(A[:, :, ev], B[:, :, ev], g, cap, oct_, passes, mag, huber_iters)
            Hm_e = _estimate_h_mimo(A[:, :, ev], B[:, :, ev], octave, huber_iters)
            Id = Hd_e[:, :, None] * B[:, :, od]
            Im = np.einsum('cdb,dbt->cbt', Hm_e, B[:, :, od])
            dom = np.abs(Id) * cap > np.abs(A[:, :, od])
            rd = np.sum(np.where(dom, np.abs(A[:, :, od] - Id) ** 2, 0))
            rm = np.sum(np.where(dom, np.abs(A[:, :, od] - Im) ** 2, 0))
            if rm < 0.8 * rd:
                I = np.einsum('cdb,dbt->cbt', _estimate_h_mimo(A, B, octave, huber_iters), B)
                mimo_used = True
        mag_i = np.abs(I)
    else:
        raise ValueError(eq)

    level_applied = False
    if track:
        c = _level_track(A, I, cap)
        if track != 'auto' or _level_gain(A, I, cap, _level_track(A, I, cap, bins=slice(0, None, 2)),
                                          level_even) > lvl_gain:
            I = I * c[:, None, :]
            mag_i = np.abs(I)
            level_applied = True

    V = A - I
    if decision == 'soft':
        # Extractable Level scales how hard the model error is suppressed (1 at 3.0's
        # default 1.2); Extraction Priority doubles it
        strength = kvol / 1.2 * (1.0 if quality else 2.0)
        if soft_q == 'gated':
            # The model error is read off a quantile of |A - I|^2 / |I|^2 in the cells the
            # instrumental dominates, which still hold some vocal. Where the model can be
            # exact (no dynamics difference, a lag that is a line) a low quantile finds the
            # model error under the vocal; where the releases' dynamics differ or the lag
            # wobbles, the error is larger and spread, and the median measures it better.
            line = align != 'track' or is_line
            # Lossy coding (a coder's low-pass in either file) leaves coding noise that
            # differs between the two files: the model cannot be exact either
            coded = lossy(A, rate) or lossy(B, rate)
            inexact = level_applied or not line or coded
            q = 0.5 if inexact else soft_q_lo
            cut = _lowpass_cut(A, B, rate) if (eq_fine and noref) else None
            f_hi = 10000.0 if cut is None else min(10000.0, cut * rate / N / 2 ** 0.25)
            if eq_fine and not inexact and eq == 'perbin' and not mimo_used and _eq_shaped(Hd, rate, f_hi):
                # 4.4: where the model can otherwise be exact, an EQ difference with shape
                # (a mastering EQ) is matched at the finer resolution `eq_fine` (octaves):
                # 1/3-octave smoothing cannot follow it exactly, and what it misses stays in
                # the vocal. A flat EQ keeps 1/3 octave, where finer detail only adds noise.
                # The finer fit must also predict held-out frames at least as well (fitted on
                # the even frames, judged on the odd frames' cells the instrumental dominates):
                # where the vocal and the accompaniment share notes, it can follow a bias.
                ev, od = slice(0, None, 2), slice(1, None, 2)
                Ic = _estimate_h(A[:, :, ev], B[:, :, ev], g, cap, oct_, passes, mag, huber_iters)[:, :, None] * B[:, :, od]
                If = _estimate_h(A[:, :, ev], B[:, :, ev], g, cap, eq_fine, passes, mag, huber_iters)[:, :, None] * B[:, :, od]
                dom = np.abs(Ic) * cap > np.abs(A[:, :, od])
                rc = np.sum(np.where(dom, np.abs(A[:, :, od] - Ic) ** 2, 0))
                rf = np.sum(np.where(dom, np.abs(A[:, :, od] - If) ** 2, 0))
                if rf < rc:
                    Hd = _estimate_h(A, B, g, cap, eq_fine, passes, mag, huber_iters)
                    I = Hd[:, :, None] * B
                    V = A - I
            if not inexact and eq_q and eq == 'perbin' and not mimo_used:
                # an EQ difference with shape (not just a level; more than 1.5 dB off the
                # median between 100 Hz and 10 kHz): 1/3-octave smoothing cannot follow it
                # exactly, so the model error is read a little higher
                f = np.arange(Hd.shape[1]) * rate / N
                db = 20 * np.log10(np.abs(Hd[:, (f > 100) & (f < 10000)]) + 1e-12)
                shape = np.max(np.abs(db - np.median(db, axis=1, keepdims=True)))
                if shape > 1.5:
                    q = eq_q
        else:
            q = soft_q
        # 4.4: the decision smoothed in the vocal's pauses, for releases whose dynamics
        # differ (level tracking on) and pairs where both files are lossy (each its own
        # coding noise)
        sm = smooth if smooth and (level_applied or (lossy(A, rate) and lossy(B, rate))) else None
        out = V * _soft_gain(A, I, cap, q, strength, rate, sm)
        if noref:
            # (the vocal's share below the cut from the cells' own decisions)
            ref = V * _soft_gain(A, I, cap, q, strength, rate) if sm else out
            out = _lowpass_share(out, A, B, rate, ref=ref)
        return _istft(out, n)
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
    # Utagoe Rip 4.0 (identical to proto-eq-align-auto)
    'v4': dict(eq='perbin', align='track', track='auto'),
    # 4.1: EQ by Huber-weighted least squares, and a soft decision per cell instead of
    # 3.0's keep-or-delete rule
    'v41': dict(eq='perbin', align='track', track='auto', mag='huber', huber_iters=2, decision='soft', soft_q='gated',
                soft_q_lo=0.10, const_lag=0.1),
    # 4.3: the 2x2 EQ for a different stereo image, a band-limited karaoke's top band by
    # the vocal's share, the low-band start for wow where it fits clearly better
    'v43': dict(eq='perbin', align='track', track='auto', mag='huber', huber_iters=2, decision='soft', soft_q='gated',
                soft_q_lo=0.10, const_lag=0.1, mimo=True, noref=True, wow=True),
    # 4.4: level tracking's test on cells chosen by the even bin below; a shaped EQ
    # difference matched at 1/6 octave where the model can otherwise be exact; the soft
    # decision smoothed over 3 x 3 cells in the vocal's pauses where the releases' dynamics
    # differ or both files are lossy
    'v44': dict(eq='perbin', align='track', track='auto', mag='huber', huber_iters=2, decision='soft', soft_q='gated',
                soft_q_lo=0.10, const_lag=0.1, mimo=True, noref=True, wow=True, level_even=True, lvl_gain=0.09,
                eq_fine=1 / 6, smooth=(3, 3, 2.0)),
    # tried for 4.3: the model error read at 0.25 where the EQ difference has shape (less
    # leftover instrumental on album_eq, but up to 4.8 dB less vocal on some MUSDB songs)
    'v43-eqq': dict(eq='perbin', align='track', track='auto', mag='huber', huber_iters=2, decision='soft',
                    soft_q='gated', soft_q_lo=0.10, const_lag=0.1, mimo=True, eq_q=0.25, noref=True, wow=True),
    # tried: the EQ model chosen per song out of sample (1/3 octave, 1 octave or one
    # gain). No difference on the dev set, so not part of v4
    'v4-cv': dict(eq='perbin', align='track', track='auto', octave='cv'),
}
