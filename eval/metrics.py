"""Scores for a separated vocal against the known vocal in the mix.

The first and last second are ignored (every engine has start-up and tail effects
there). Read `leak` only next to an SDR: an engine that deletes everything also
leaks nothing.
"""
from __future__ import annotations

import numpy as np

RATE = 44100
EDGE = RATE          # samples ignored at each end


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
    """Median SDR over 1 s windows where the vocal is singing (BSSEval-style).
    The headline number."""
    w = _windows(len(ref), win)
    e = np.array([np.sum(ref[a:b] ** 2) for a, b in w])
    keep = e > e.max() * 10 ** (active_db / 10)
    vals = [_db(np.sum(ref[a:b] ** 2), np.sum((ref[a:b] - est[a:b]) ** 2)) for (a, b), k in zip(w, keep) if k]
    return float(np.median(vals)) if vals else float('nan')


def leak(ref, est, inst, win=RATE // 10, quiet_db=-40.0):
    """Error where the vocal is (nearly) silent, relative to the instrumental there
    (dB, lower is better). Error rather than output energy, so a perfect separator
    scores -inf however much reverb tail the target has."""
    w = _windows(len(ref), win)
    e = np.array([np.sum(ref[a:b] ** 2) for a, b in w])
    quiet = [(a, b) for (a, b), x in zip(w, e) if x < e.max() * 10 ** (quiet_db / 10)]
    num = sum(np.sum((est[a:b] - ref[a:b]) ** 2) for a, b in quiet)
    den = sum(np.sum(inst[a:b] ** 2) for a, b in quiet)
    return _db(num, den) if den > 0 else float('nan')


def score(target, est, mix):
    n = min(len(target), len(est))
    sl = slice(EDGE, n - EDGE) if n > 4 * EDGE else slice(0, n)
    t, e, m = target[sl], est[sl], mix[sl]
    return {
        'median_sdr': median_sdr(t, e),
        'sdr': sdr(t, e),
        'si_sdr': si_sdr(t, e),
        'leak': leak(t, e, m - t),
    }
