"""Test scenarios: ways the album mix and the karaoke file differ in real releases.

Every scenario returns (mix, karaoke, target). `target` is the vocal exactly as it
sounds inside `mix`, so a perfect separator would return it sample for sample. The
mastering chain is an EQ (linear) followed by gain envelopes (compressor, limiter)
computed on the whole mix, so the vocal inside the mix is gain(t) * EQ(vocal).
"""
from __future__ import annotations

import os
import subprocess
import tempfile

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


def eq(x, bands=MASTER_EQ):
    for kind, f0, g, q in bands:
        x = signal.lfilter(*_biquad(kind, f0, g, q), x, axis=0)
    return x


def compressor_gain(x, threshold_db=-20.0, ratio=3.0, attack=0.010, release=0.150, makeup_db=4.0, rate=RATE):
    """Linked stereo feed-forward compressor; returns the gain envelope (n,)."""
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
    lev_db = 20 * np.log10(np.maximum(env, 1e-9))
    over = np.maximum(lev_db - threshold_db, 0)
    g_db = -over * (1 - 1 / ratio) + makeup_db
    g = 10 ** (g_db / 20)
    return np.interp(np.arange(len(level)), np.arange(n_blk) * hop + hop / 2, g)


def limiter_gain(x, ceiling=0.89, lookahead=0.0015, rate=RATE):
    """Look-ahead brickwall limiter gain envelope: never lets |x * g| exceed `ceiling`."""
    L = max(1, int(lookahead * rate))
    need = np.minimum(1.0, ceiling / np.maximum(np.abs(x).max(axis=1), 1e-12))
    g = ndimage.minimum_filter1d(need, 2 * L + 1)
    return ndimage.uniform_filter1d(g, L + 1)


def master(vocal, inst, comp=True, limit=True):
    """Album mastering applied to the whole mix. Returns (mix, vocal as heard in the mix)."""
    v, i = eq(vocal), eq(inst)
    g = np.ones(len(v))
    if comp:
        g = g * compressor_gain(v + i)
    if limit:
        g = g * limiter_gain((v + i) * g[:, None])
    return (v + i) * g[:, None], v * g[:, None]


def delay(x, samples):
    """Delay by a fractional number of samples (band-limited, via the FFT)."""
    n = len(x) + int(np.ceil(abs(samples))) + 64
    X = np.fft.rfft(x, n=n, axis=0)
    w = np.exp(-2j * np.pi * np.fft.rfftfreq(n) * samples)
    return np.fft.irfft(X * w[:, None], n=n, axis=0)[: len(x)]


def drift(x, ppm):
    """Play `x` slightly fast/slow (clock drift between the two releases)."""
    y = signal.resample(x, int(round(len(x) * (1 + ppm * 1e-6))), axis=0)[: len(x)]
    return np.pad(y, ((0, len(x) - len(y)), (0, 0)))


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


# ---------------------------------------------------------------- scenarios
def _plain(v, i):
    return v + i, i, v


SCENARIOS = {
    # identical instrumental, perfectly aligned: the ideal case
    'clean': lambda v, i: _plain(v, i),
    # karaoke released at a different level
    'level': lambda v, i: (v + 0.8 * i, i, v),
    # album mastered with EQ only (the karaoke is the unmastered instrumental)
    'master_eq': lambda v, i: (*_swap(master(v, i, comp=False, limit=False), i),),
    # EQ + bus compression + limiting on the album
    'master_full': lambda v, i: (*_swap(master(v, i), i),),
    # karaoke starts 1234.37 samples later than the album
    'offset_frac': lambda v, i: (v + i, delay(i, 1234.37), v),
    # karaoke runs 30 ppm fast (about 40 samples over 30 s)
    'drift': lambda v, i: (v + i, drift(delay(i, 512), 30), v),
    # both files went through 192 kbps MP3
    'mp3': lambda v, i: (mp3(v + i), mp3(i), v),
    # all of the above at once
    'everything': lambda v, i: _everything(v, i),
}


def _swap(mixed, i):
    mix, target = mixed
    return mix, i, target


def _everything(v, i):
    mix, target = master(v, i)
    kar = drift(delay(i * 0.9, 777.6), -20)
    return mp3(mix), mp3(kar), target
