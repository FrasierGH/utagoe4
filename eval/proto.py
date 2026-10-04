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
def separate(mix, kar, rate=44100, eq='scalar', track=False, kvol=1.2, quality=True, octave=1 / 3, passes=2):
    """Returns the vocal estimate (same shape as mix)."""
    kar = np.pad(kar, ((0, max(0, len(mix) - len(kar))), (0, 0)))[: len(mix)]   # files may differ in length
    lag = find_lag(mix, kar, rate)
    k = shift(kar, lag)
    cap = min(kvol, 1.5) if quality else kvol
    A, B = _stft(mix), _stft(k)                                   # (ch, bins, frames)

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
}
