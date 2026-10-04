"""Prototype separator: a whole-file version of Utagoe Rip's spectral subtraction
with the improvements under test as switches.

The decision rule is 3.0's ThVocalFFT (FFT 8192, hop 1024, Hann; zero a bin when the
instrumental explains it: |I|*cap > |O|, and in quality mode the phases agree within
thr[k]). What changes is how the instrumental estimate I is made:

    eq='scalar'   one gain for the whole file (what 3.0 does, per block)
    eq='perbin'   a complex gain per frequency bin, H(f) = sum O conj(K) / sum |K|^2,
                  smoothed over `octave` (1/3): matches the album's EQ, and any small
                  residual delay (a linear phase) for free. With passes=2 the second
                  estimate only uses the cells the first says the instrumental
                  dominates, which keeps the vocal from adding noise to H.
    track=True    plus a per-frame level correction (tried for compression; it made
                  results worse, see eval/README.md)
"""
from __future__ import annotations

import numpy as np
from scipy import signal

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
def find_lag(mix, kar, rate, max_sec=10.0):
    """Integer lag L with kar[t + L] ~ mix[t] (GCC-PHAT on the mono sums)."""
    a, b = mix.mean(1), kar.mean(1)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    X = np.fft.rfft(b, n) * np.conj(np.fft.rfft(a, n))
    r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)
    m = int(max_sec * rate)
    lags = np.concatenate([np.arange(0, m), np.arange(-m, 0)])
    vals = np.concatenate([r[:m], r[-m:]])
    return int(lags[np.argmax(vals)])


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


def track_lags(mix, kar, rate, lag0, win_sec=4.0, hop_sec=2.0, search=256):
    """Local lag (in samples, fractional) of kar against mix in overlapping windows.

    Returns (centres, lags, weights). Each window: GCC-PHAT for the integer lag within
    lag0 +- search, then the slope of the cross-spectrum phase (100 Hz - 8 kHz,
    weighted by magnitude) for the fraction. The weight is the PHAT peak height.
    """
    a, b = mix.mean(1), kar.mean(1)
    W, H = int(win_sec * rate), int(hop_sec * rate)
    win = np.hanning(W)
    n = 1 << int(np.ceil(np.log2(2 * W)))
    f = np.fft.rfftfreq(n, 1 / rate)
    band = (f > 100) & (f < 8000)
    centres, lags, weights = [], [], []
    for s in range(0, max(1, len(a) - W + 1), H):
        sa = a[s:s + W] * win
        sb = _segment(b, s + lag0, W) * win
        if np.sum(sa ** 2) < 1e-10 or np.sum(sb ** 2) < 1e-10:
            continue
        X = np.fft.rfft(sb, n) * np.conj(np.fft.rfft(sa, n))
        r = np.fft.irfft(X / (np.abs(X) + 1e-12), n)
        cand = np.concatenate([np.arange(0, search + 1), np.arange(-search, 0)])
        vals = np.concatenate([r[: search + 1], r[-search:]])
        d = int(cand[np.argmax(vals)])
        # fractional part: sb lags sa by D = d + frac samples, so the cross-spectrum
        # phase is -omega*D; take out d and fit the remaining slope
        Y = X * np.exp(2j * np.pi * f * d / rate)
        ph, w = np.angle(Y[band]), np.abs(Y[band])
        om = 2 * np.pi * f[band] / rate
        frac = -np.sum(w * om * ph) / np.sum(w * om * om)
        centres.append(s + W / 2)
        lags.append(lag0 + d + frac)
        weights.append(float(np.max(vals)))
    return np.array(centres), np.array(lags), np.array(weights)


def fit_lag_curve(centres, lags, weights, n):
    """Lag for every sample: a weighted straight line (clock drift) with outliers
    removed; if a line doesn't fit, a smoothed piecewise-linear curve."""
    t = np.arange(n)
    if len(lags) == 0:
        return np.zeros(n)
    if len(lags) < 3:
        return np.full(n, np.median(lags))
    keep = np.ones(len(lags), bool)
    for _ in range(3):
        c = np.polyfit(centres[keep], lags[keep], 1, w=np.sqrt(weights[keep]))
        res = np.abs(np.polyval(c, centres) - lags)
        new = res < max(0.5, 3 * np.median(res[keep]))
        if new.sum() < 3 or (new == keep).all():
            break
        keep = new
    if np.median(res[keep]) < 0.25:
        return np.polyval(c, t)
    from scipy.ndimage import median_filter
    sm = median_filter(lags, size=5, mode='nearest')
    return np.interp(t, centres, sm)


_KERNELS = {}


def _kernel_table(half, phases=1024, beta=9.0):
    """Kaiser-windowed sinc taps for `phases` fractional positions: (phases + 1, 2 * half)."""
    key = (half, phases, beta)
    if key not in _KERNELS:
        fr = np.arange(phases + 1)[:, None] / phases
        u = np.arange(-half + 1, half + 1)[None, :] - fr
        _KERNELS[key] = np.sinc(u) * np.i0(beta * np.sqrt(np.clip(1 - (u / half) ** 2, 0, None))) / np.i0(beta)
    return _KERNELS[key]


def frac_read(x, pos, half=32, phases=1024):
    """x sampled at fractional positions `pos` (Kaiser-windowed sinc, 2*half taps; the
    fraction is quantised to 1/phases of a sample, an error far below 16-bit noise)."""
    tab = _kernel_table(half, phases)
    i0 = np.floor(pos).astype(np.int64)
    q = np.rint((pos - i0) * phases).astype(np.int64)
    xp = np.pad(x, ((half, half + 1), (0, 0)))
    base = np.clip(i0 + half, 0, len(xp) - 2 * half - 1)
    out = np.zeros((len(pos), x.shape[1]))
    for j, k in enumerate(range(-half + 1, half + 1)):
        out += xp[base + k] * tab[q, j][:, None]
    return out


def align_track(mix, kar, rate, lag0, passes=2):
    """The lag of kar against mix at every sample: a drifting, sub-sample curve. The
    second pass measures what is left after the first (nearly constant, so the
    windows aren't smeared by drift). Returns (lag curve, kar read along it)."""
    n = len(mix)
    t = np.arange(n, dtype=float)
    lag_t = fit_lag_curve(*track_lags(mix, kar, rate, lag0), n)
    k = frac_read(kar, t + lag_t)
    for _ in range(passes - 1):
        res = fit_lag_curve(*track_lags(mix, k, rate, 0, search=8), n)
        lag_t = lag_t + res
        k = frac_read(kar, t + lag_t)
    return lag_t, k


def _stft_along(x, lag_t, n_frames):
    """STFT of x (n, ch) with frame k taken at its own lag: the frame centred on
    k*HOP + lag(k*HOP). The integer part picks the samples, the fraction is a linear
    phase on the frame's spectrum, so nothing is resampled. Matches _stft's scaling
    and framing (centred frames, as scipy.signal.stft with boundary='zeros')."""
    win = signal.get_window('hann', N)
    centres = np.arange(n_frames) * HOP
    lag = lag_t[np.clip(centres, 0, len(lag_t) - 1)]
    li = np.floor(lag).astype(np.int64)
    fr = lag - li
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


# ---------------------------------------------------------------- helpers
def _smooth_bins(z, octave):
    """Sum over a band of +-octave/2 around every bin (at least +-2 bins), along the last axis."""
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


# ---------------------------------------------------------------- the separator
def separate(mix, kar, rate=44100, eq='scalar', track=False, kvol=1.2, quality=True, octave=1 / 3, passes=2,
             align='global', mag='power'):
    """Returns the vocal estimate (same shape as mix)."""
    kar_full = kar
    kar = np.pad(kar, ((0, max(0, len(mix) - len(kar))), (0, 0)))[: len(mix)]   # files may differ in length
    lag = find_lag(mix, kar, rate)
    A = _stft(mix)                                                # (ch, bins, frames)
    if align == 'global':           # one integer offset for the whole file
        k = shift(kar, lag)
        B = _stft(k)
    elif align == 'track':          # sub-sample offset that may change over the song (drift)
        lag_t, k = align_track(mix, kar_full, rate, lag)
        B = _stft_along(kar_full, lag_t, A.shape[2])
    else:
        raise ValueError(align)
    cap = min(kvol, 1.5) if quality else kvol

    if eq == 'scalar':
        g = np.sum(mix * k) / max(np.sum(k * k), 1e-20)
        I = min(g, cap) * B
        mag = np.abs(B)                                           # 3.0 tests the raw instrumental
    elif eq == 'perbin':
        # passes > 1: re-estimate from the cells the previous estimate says the instrumental
        # dominates, so the vocal adds less noise to H
        g = np.sum(mix * k) / max(np.sum(k * k), 1e-20)
        H = np.full(A.shape[:2], g, dtype=complex)
        w = np.ones(A.shape, dtype=bool)
        for p in range(passes):
            if p:
                w = np.abs(H[:, :, None] * B) * cap > np.abs(A)
            num = np.sum(np.where(w, A * np.conj(B), 0), axis=2)   # (ch, bins)
            den = np.sum(np.where(w, np.abs(B) ** 2, 0), axis=2)
            H = _smooth_bins(num, octave) / (_smooth_bins(den, octave) + 1e-20)
            if mag == 'power' and p:
                # least squares is biased low when the karaoke has noise of its own (MP3
                # coding noise): take |H| from the power ratio of the instrumental-
                # dominated cells instead, and keep the phase of the cross-spectrum
                pa = _smooth_bins(np.sum(np.where(w, np.abs(A) ** 2, 0), axis=2), octave)
                H = np.sqrt(pa / (_smooth_bins(den, octave) + 1e-20)) * np.exp(1j * np.angle(H))
        I = H[:, :, None] * B
        mag = np.abs(I)
    else:
        raise ValueError(eq)

    if track:  # per-frame level, least squares over all bins, lightly smoothed in time
        c = np.sum(np.real(A * np.conj(I)), axis=1) / (np.sum(np.abs(I) ** 2, axis=1) + 1e-20)   # (ch, frames)
        c = signal.filtfilt(np.ones(3) / 3, [1], c, axis=1) if c.shape[1] > 9 else c
        I = I * c[:, None, :]
        mag = np.abs(I)

    V = A - I
    kill = mag * cap > np.abs(A)
    if quality:
        kill &= _phase_diff(A, I) < _thr(kvol)[None, :, None]
    V[kill] = 0
    return _istft(V, len(mix))


ENGINES = {
    'proto-scalar': dict(eq='scalar'),     # 3.0's approach, whole file: checks the prototype against 3.0
    'proto-eq': dict(eq='perbin'),         # improvement 1: per-band EQ matching
    'proto-eq-align': dict(eq='perbin', align='track'),   # + 2: drift-tracking, sub-sample alignment
}
