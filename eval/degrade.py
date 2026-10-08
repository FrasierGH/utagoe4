"""Test scenarios: ways the album mix and the karaoke file differ in real releases.

Every scenario returns a Case: mix, karaoke, and `target`, the vocal as it sounds
inside the mix. A mastering chain is an EQ (linear) followed by gain envelopes
(compressor, limiter) computed on that whole release, so the vocal inside a mastered
mix is exactly gain(t) * EQ(vocal). MP3 is the exception: coding noise is not
separable, so there the target is only approximate and the case also carries a
`ceiling`, the result of subtracting the coded karaoke perfectly.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
from dataclasses import dataclass, field

import numpy as np
import soundfile as sf
from scipy import ndimage, signal

RATE = 44100


# ---------------------------------------------------------------- building blocks
def _biquad(kind, f0, gain_db, q, rate=RATE):
    """RBJ cookbook biquad (b, a)."""
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * f0 / rate
    cw, sw = np.cos(w), np.sin(w)
    alpha = sw / (2 * q)
    if kind == 'peak':
        b = [1 + alpha * A, -2 * cw, 1 - alpha * A]
        a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    elif kind in ('lowshelf', 'highshelf'):
        s = 1 if kind == 'lowshelf' else -1
        sq = 2 * np.sqrt(A) * alpha
        b = [A * ((A + 1) - s * (A - 1) * cw + sq), s * 2 * A * ((A - 1) - s * (A + 1) * cw), A * ((A + 1) - s * (A - 1) * cw - sq)]
        a = [(A + 1) + s * (A - 1) * cw + sq, -s * 2 * ((A - 1) + s * (A + 1) * cw), (A + 1) + s * (A - 1) * cw - sq]
    else:
        raise ValueError(kind)
    return np.array(b) / a[0], np.array(a) / a[0]


MASTER_EQ = [('lowshelf', 110, 3.0, 0.7), ('peak', 350, -2.0, 1.0), ('peak', 2800, 2.5, 0.9), ('highshelf', 9000, 3.5, 0.7)]
MASTER_EQ_B = [('lowshelf', 80, 1.5, 0.7), ('peak', 600, 1.5, 1.2), ('peak', 4500, -1.5, 1.0), ('highshelf', 12000, 2.0, 0.7)]


def eq(x, bands=MASTER_EQ):
    for kind, f0, g, q in bands:
        x = signal.lfilter(*_biquad(kind, f0, g, q), x, axis=0)
    return x


def _db(x):
    return 20 * np.log10(np.maximum(x, 1e-12))


def compressor_gain(x, threshold_db, ratio, attack=0.005, release=0.120, rate=RATE):
    """Linked stereo feed-forward compressor (peak detector); returns the gain envelope (n,)."""
    hop = 32
    level = np.abs(x).max(axis=1)
    n_blk = (len(level) + hop - 1) // hop
    blk = np.pad(level, (0, n_blk * hop - len(level))).reshape(n_blk, hop).max(axis=1)
    a_att = np.exp(-hop / (attack * rate))
    a_rel = np.exp(-hop / (release * rate))
    env = np.empty(n_blk)
    e = 0.0
    for i, v in enumerate(blk):
        e = a_att * e + (1 - a_att) * v if v > e else a_rel * e + (1 - a_rel) * v
        env[i] = e
    over = np.maximum(_db(env) - threshold_db, 0)
    g = 10 ** (-over * (1 - 1 / ratio) / 20)
    return np.interp(np.arange(len(level)), np.arange(n_blk) * hop + hop / 2, g)


def limiter_gain(x, ceiling=0.89, lookahead=0.0015, release=0.050, rate=RATE):
    """Look-ahead brickwall limiter gain envelope: never lets |x * g| exceed `ceiling`.
    Attack is instant (via the look-ahead); the gain recovers with `release`."""
    L = max(1, int(lookahead * rate))
    need = np.minimum(1.0, ceiling / np.maximum(np.abs(x).max(axis=1), 1e-12))
    g = ndimage.minimum_filter1d(need, 2 * L + 1)
    hop = 32
    n_blk = (len(g) + hop - 1) // hop
    blk = np.pad(g, (0, n_blk * hop - len(g)), constant_values=1.0).reshape(n_blk, hop).min(axis=1)
    a = np.exp(-hop / (release * rate))
    rel = np.empty(n_blk)
    e = 1.0
    for i, v in enumerate(blk):
        e = v if v < e else a * e + (1 - a) * v
        rel[i] = e
    g = np.minimum(g, np.repeat(rel, hop)[: len(g)])
    # smoothing by a moving average over a window no longer than the minimum filter's
    # keeps the gain at or below what every peak needs
    return ndimage.uniform_filter1d(ndimage.minimum_filter1d(g, 2 * L + 1), L + 1)


@dataclass
class Chain:
    """A mastering chain. Levels are relative to the signal, so it acts the same on
    every song: the compressor threshold sits `thr_over_rms` dB above the release's
    RMS level, and the limiter is driven so the peaks would overshoot by `drive_db`."""
    bands: list
    thr_over_rms: float = 0.0
    ratio: float = 1.0
    drive_db: float = 0.0
    ceiling: float = 0.89


EQ_ONLY = Chain(MASTER_EQ)
LOUD = Chain(MASTER_EQ, thr_over_rms=-2.0, ratio=4.0, drive_db=6.0)        # a modern loud master
LOUD_B = Chain(MASTER_EQ_B, thr_over_rms=2.0, ratio=2.5, drive_db=3.0)     # a different engineer


def master(stems, chain):
    """Master one release made of `stems` (the envelopes see their sum). Returns the
    processed stems, each EQ'd and multiplied by the shared gain envelope, and stats
    on how much the dynamics processing moved the gain."""
    eqd = [eq(s, chain.bands) for s in stems]
    total = sum(eqd)
    g = np.ones(len(total))
    if chain.ratio > 1:
        rms_db = 10 * np.log10(np.mean(total ** 2))
        g = compressor_gain(total, rms_db + chain.thr_over_rms, chain.ratio)
    if chain.drive_db > 0:
        y = total * g[:, None]
        pre = chain.ceiling / np.max(np.abs(y)) * 10 ** (chain.drive_db / 20)
        g = g * pre * limiter_gain(y * pre, chain.ceiling)
    gdb = _db(g / np.median(g))
    stats = {'gain_std_db': float(np.std(gdb)),
             'gain_p1_p99_db': float(np.percentile(gdb, 99) - np.percentile(gdb, 1))}
    return [s * g[:, None] for s in eqd], stats


def delay(x, samples):
    """Delay by a fractional number of samples (band-limited, via the FFT)."""
    n = len(x) + int(np.ceil(abs(samples))) + 64
    X = np.fft.rfft(x, n=n, axis=0)
    w = np.exp(-2j * np.pi * np.fft.rfftfreq(n) * samples)
    return np.fft.irfft(X * w[:, None], n=n, axis=0)[: len(x)]


def drift(x, ppm):
    """Change the playback rate by `ppm` (clock drift between releases). Positive ppm
    stretches the file: it plays slower, so it falls further behind over time."""
    y = signal.resample(x, int(round(len(x) * (1 + ppm * 1e-6))), axis=0)[: len(x)]
    return np.pad(y, ((0, len(x) - len(y)), (0, 0)))


def warp(x, d, half=64, beta=10.0):
    """Time-varying delay: y[t] = x(t - d[t]), Kaiser-windowed sinc with 2*half taps.
    Deliberately written independently of the separator's resampler."""
    pos = np.arange(len(x)) - d
    i0 = np.floor(pos).astype(np.int64)
    fr = pos - i0
    xp = np.pad(x, ((half, half + 1), (0, 0)))
    out = np.zeros(x.shape)
    for k in range(-half + 1, half + 1):
        u = k - fr
        w = np.sinc(u) * np.i0(beta * np.sqrt(np.clip(1 - (u / half) ** 2, 0, None))) / np.i0(beta)
        idx = i0 + k + half
        ok = (idx >= 0) & (idx < len(xp))
        out[ok] += xp[idx[ok]] * w[ok, None]
    return out


def wow_curve(n, rate=RATE):
    """Delay with tape/vinyl-style speed wobble: +-6 samples at 0.55 Hz (a 33 rpm
    record) plus a slow +-4 sample wander, around 400 samples."""
    t = np.arange(n) / rate
    return 400 + 6 * np.sin(2 * np.pi * 0.55 * t) + 4 * np.sin(2 * np.pi * t / 11 + 1.0)


def mp3(x, kbps=192, rate=RATE):
    """Round trip through LAME (gapless: the decoder removes encoder delay and padding)."""
    with tempfile.TemporaryDirectory() as d:
        src, enc, dec = (os.path.join(d, f) for f in ('a.wav', 'a.mp3', 'b.wav'))
        sf.write(src, np.clip(x, -1, 1), rate, subtype='PCM_16')
        subprocess.run(['lame', '--quiet', '-b', str(kbps), src, enc], check=True)
        subprocess.run(['lame', '--quiet', '--decode', enc, dec], check=True)
        y, _ = sf.read(dec)
    out = np.zeros_like(x)
    out[: min(len(x), len(y))] = y[: len(x)]
    return out


def lowpass_brick(x, hz=16000.0, width=300.0, rate=RATE):
    """A lossy coder's low-pass without its coding noise: zero phase, flat to `hz` - width,
    a raised-cosine fall over `width`, nothing above `hz`."""
    n = len(x)
    X = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(n, 1 / rate)
    g = np.clip((hz - f) / width, 0, 1)
    g = 0.5 - 0.5 * np.cos(np.pi * g)
    return np.fft.irfft(X * g[:, None], n, axis=0)


def narrower(x, side=0.6):
    """The stereo image narrowed: mid kept, side scaled."""
    m, s = (x[:, 0] + x[:, 1]) / 2, (x[:, 0] - x[:, 1]) / 2
    return np.stack([m + side * s, m - side * s], axis=1)


# ---------------------------------------------------------------- scenarios
@dataclass
class Case:
    mix: np.ndarray
    kar: np.ndarray
    target: np.ndarray
    ceiling: np.ndarray = None      # best achievable output where the target is approximate
    stats: dict = field(default_factory=dict)


def _album(v, i, chain):
    (mv, mi), st = master([v, i], chain)
    return Case(mv + mi, i, mv, stats={'album': st})


def _both(v, i, chain_album, chain_kar):
    (mv, mi), st = master([v, i], chain_album)
    (ki,), st_k = master([i], chain_kar)
    return Case(mv + mi, ki, mv, stats={'album': st, 'karaoke': st_k})


def _mp3(v, i):
    mix, kar = mp3(v + i), mp3(i)
    return Case(mix, kar, v, ceiling=mix - kar)


def _everything(v, i):
    c = _both(v, i, LOUD, LOUD_B)
    c.kar = drift(delay(c.kar * 0.9, 777.6), -20)
    c.mix, c.kar = mp3(c.mix), mp3(c.kar)
    return c


SCENARIOS = {
    # the ideal case: identical instrumental, aligned
    'clean': lambda v, i: Case(v + i, i, v),
    # karaoke released at a different level
    'level': lambda v, i: Case(v + 0.8 * i, i, v),
    # karaoke with inverted polarity
    'inverted': lambda v, i: Case(v + i, -i, v),
    # album EQ'd, karaoke raw; no dynamics processing
    'album_eq': lambda v, i: _album(v, i, EQ_ONLY),
    # album loudly mastered (compressor + limiter), karaoke raw
    'album_loud': lambda v, i: _album(v, i, LOUD),
    # the usual commercial case: both releases through the same chain, each with its
    # own gain envelope (the karaoke's has no vocal pushing it down)
    'both_loud': lambda v, i: _both(v, i, LOUD, LOUD),
    # a remaster: the karaoke mastered separately, with another EQ and chain
    'both_diff': lambda v, i: _both(v, i, LOUD, LOUD_B),
    # karaoke 1234.37 samples late
    'offset_frac': lambda v, i: Case(v + i, delay(i, 1234.37), v),
    # karaoke plays 30 ppm slow: 40 samples behind after 30 s
    'drift': lambda v, i: Case(v + i, drift(delay(i, 512), 30), v),
    # 300 ppm: 400 samples behind after 30 s
    'drift_fast': lambda v, i: Case(v + i, drift(delay(i, 512), 300), v),
    # vinyl/tape speed wobble
    'wow': lambda v, i: Case(v + i, warp(i, wow_curve(len(i))), v),
    # both through 192 kbps MP3 (approximate target: compare with the ceiling)
    'mp3': _mp3,
    # remaster + level + offset + drift + MP3 (approximate target)
    'everything': _everything,
    # a band-limited karaoke (a YouTube or low-bitrate rip): nothing above 16 kHz
    'kar_lowpass': lambda v, i: Case(v + i, lowpass_brick(i), v),
    # the karaoke's stereo image narrower (another mix or master): side at 60 %
    'stereo_width': lambda v, i: Case(v + i, narrower(i), v),
}
