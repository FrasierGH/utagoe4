"""Scores for a separated vocal against the known vocal in the mix (higher is better, except bleed)."""
from __future__ import annotations

import numpy as np

RATE = 44100


def _db(num, den):
    return 10 * np.log10(max(num, 1e-20) / max(den, 1e-20))


def sdr(ref, est):
    """Signal-to-distortion ratio over the whole file (dB)."""
    return _db(np.sum(ref ** 2), np.sum((ref - est) ** 2))


def si_sdr(ref, est):
    """Scale-invariant SDR, averaged over channels (dB): ignores an overall gain error."""
    out = []
    for c in range(ref.shape[1]):
        r, e = ref[:, c], est[:, c]
        a = np.dot(e, r) / max(np.dot(r, r), 1e-20)
        out.append(_db(np.sum((a * r) ** 2), np.sum((a * r - e) ** 2)))
    return float(np.mean(out))


def _windows(n, win):
    return [(s, min(n, s + win)) for s in range(0, n - win // 2, win)]


def median_sdr(ref, est, win=RATE, active_db=-30.0):
    """Median SDR over 1 s windows where the vocal is singing (BSSEval-style)."""
    e = np.array([np.sum(ref[a:b] ** 2) for a, b in _windows(len(ref), win)])
    keep = e > e.max() * 10 ** (active_db / 10)
    vals = [_db(np.sum(ref[a:b] ** 2), np.sum((ref[a:b] - est[a:b]) ** 2))
            for (a, b), k in zip(_windows(len(ref), win), keep) if k]
    return float(np.median(vals)) if vals else float('nan')


def bleed(ref, est, inst, win=RATE // 10, silent_db=-40.0):
    """Instrumental left in the output where the vocal is silent, relative to the
    instrumental itself (dB; lower is better)."""
    e = np.array([np.sum(ref[a:b] ** 2) for a, b in _windows(len(ref), win)])
    quiet = e < e.max() * 10 ** (silent_db / 10)
    num = sum(np.sum(est[a:b] ** 2) for (a, b), q in zip(_windows(len(ref), win), quiet) if q)
    den = sum(np.sum(inst[a:b] ** 2) for (a, b), q in zip(_windows(len(ref), win), quiet) if q)
    return _db(num, den) if den > 0 else float('nan')


def score(target, est, mix):
    n = min(len(target), len(est))
    t, e = target[:n], est[:n]
    return {
        'sdr': sdr(t, e),
        'median_sdr': median_sdr(t, e),
        'si_sdr': si_sdr(t, e),
        'bleed': bleed(t, e, mix[:n] - t),
    }
