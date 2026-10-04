"""Synthetic multitrack songs: a vocal stem and an instrumental stem with known content.

Good enough to develop against (drums, bass, chords, a lead, and a formant-shaped
voice with vibrato, glides, consonants and reverb). Real stems (eval/data/) are what
results should finally be judged on.
"""
from __future__ import annotations

import numpy as np
from scipy import signal

RATE = 44100


def _env(n, attack, decay, rate=RATE):
    t = np.arange(n) / rate
    return np.minimum(t / max(attack, 1e-4), 1.0) * np.exp(-t / decay)


def _place(dst, src, at, gain=(1.0, 1.0)):
    end = min(len(dst), at + len(src))
    if end > at:
        dst[at:end, 0] += src[: end - at] * gain[0]
        dst[at:end, 1] += src[: end - at] * gain[1]


def _pan(p):  # -1 left .. 1 right, constant power
    a = (p + 1) * np.pi / 4
    return np.cos(a), np.sin(a)


def _saw(phase, harmonics):
    return sum(np.sin(k * phase) / k for k in range(1, harmonics + 1)) * (2 / np.pi)


def _reverb(x, rate, seconds, wet, rng):
    """Stereo reverb: decorrelated exponentially decaying noise impulse responses."""
    n = int(seconds * rate)
    t = np.arange(n) / rate
    out = np.zeros((len(x), 2))
    for ch in range(2):
        ir = rng.normal(0, 1, n) * np.exp(-6.9 * t / seconds)
        ir = signal.lfilter(*signal.butter(1, 6000 / (rate / 2)), ir)
        ir *= wet / np.sqrt(np.sum(ir ** 2))
        out[:, ch] = signal.fftconvolve(x, ir)[: len(x)]
    return out


# formants (Hz, bandwidth, gain) of a few vowels
VOWELS = {
    'a': [(800, 80, 1.0), (1150, 90, 0.5), (2900, 120, 0.25), (3900, 130, 0.1)],
    'e': [(400, 60, 1.0), (1600, 80, 0.3), (2700, 120, 0.25), (3300, 150, 0.1)],
    'i': [(270, 60, 1.0), (2300, 100, 0.25), (3000, 120, 0.2), (3600, 150, 0.1)],
    'o': [(450, 70, 1.0), (800, 80, 0.6), (2830, 100, 0.15), (3500, 130, 0.05)],
    'u': [(325, 50, 1.0), (700, 60, 0.3), (2530, 170, 0.1), (3500, 180, 0.05)],
}


def _formant_gain(freqs, vowel):
    g = np.full(freqs.shape, 0.02)
    for f, bw, a in VOWELS[vowel]:
        g += a / (1 + ((freqs - f) / bw) ** 2)
    return g


def song(seed: int, seconds: float = 30.0, rate: int = RATE):
    """Returns (vocal, instrumental), each float64 (n, 2)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    bpm = rng.uniform(96, 132)
    beat = int(rate * 60 / bpm)
    root = 40 + rng.integers(0, 12)                       # MIDI note of the key
    scale = np.array([0, 2, 4, 5, 7, 9, 11])
    prog = [0, 4, 5, 3] if rng.random() < 0.5 else [5, 3, 0, 4]   # scale degrees
    midi = lambda m: 440.0 * 2 ** ((m - 69) / 12)

    inst = np.zeros((n, 2))
    # drums
    kick_len = int(0.35 * rate)
    tk = np.arange(kick_len) / rate
    kick = np.sin(2 * np.pi * (50 * tk + 100 * 0.04 * (1 - np.exp(-tk / 0.04)))) * _env(kick_len, 0.002, 0.12)
    sn_len = int(0.25 * rate)
    snare = (signal.lfilter(*signal.butter(2, [900 / (rate / 2), 6000 / (rate / 2)], 'band'), rng.normal(0, 1, sn_len))
             * _env(sn_len, 0.001, 0.07) + 0.5 * np.sin(2 * np.pi * 185 * np.arange(sn_len) / rate) * _env(sn_len, 0.001, 0.05))
    hat_len = int(0.06 * rate)
    hat_src = signal.lfilter(*signal.butter(2, 7000 / (rate / 2), 'high'), rng.normal(0, 1, hat_len * 64))
    for b in range(n // beat + 1):
        at = b * beat
        if b % 2 == 0 or rng.random() < 0.15:
            _place(inst, kick * 0.9, at)
        if b % 2 == 1:
            _place(inst, snare * 0.45, at, _pan(0.1))
        for h in range(2):
            k = rng.integers(0, 63)
            _place(inst, hat_src[k * hat_len:(k + 1) * hat_len] * _env(hat_len, 0.001, 0.02) * 0.25,
                   at + h * beat // 2, _pan(0.4))

    # harmony: one chord per bar (4 beats)
    bar = 4 * beat
    lead_notes = []
    for b in range(n // bar + 1):
        deg = prog[b % len(prog)]
        chord = [root + 12 + scale[(deg + i) % 7] + 12 * ((deg + i) // 7) for i in (0, 2, 4)]
        at, ln = b * bar, min(bar, n - b * bar)
        if ln <= 0:
            break
        t = np.arange(ln) / rate
        # pad: detuned saws, wide stereo, lowpassed
        pad = np.zeros((ln, 2))
        for m in chord:
            for ch, det in ((0, -0.08), (1, 0.08)):
                pad[:, ch] += _saw(2 * np.pi * midi(m + 12 + det) * t, 12)
        pad = signal.lfilter(*signal.butter(2, 2500 / (rate / 2)), pad, axis=0) * 0.06 * _env(ln, 0.15, 8.0)[:, None]
        inst[at:at + ln] += pad
        # bass: root on eighths
        for e in range(8):
            s = at + e * beat // 2
            l = min(beat // 2, n - s)
            if l <= 0:
                break
            tb = np.arange(l) / rate
            note = _saw(2 * np.pi * midi(chord[0] - 12) * tb, 20) * _env(l, 0.003, 0.25)
            note = signal.lfilter(*signal.butter(2, 700 / (rate / 2)), note) * 0.35
            _place(inst, note, s)
        # pluck lead: arpeggio, panned left
        for e in range(8):
            s = at + e * beat // 2
            m = chord[e % 3] + 12 + (12 if e >= 4 else 0)
            l = min(int(0.4 * rate), n - s)
            if l <= 0:
                break
            tp = np.arange(l) / rate
            pl = sum(np.sin(2 * np.pi * midi(m) * k * tp) * np.exp(-tp * (6 + 3 * k)) / k for k in range(1, 9))
            _place(inst, pl * 0.12, s, _pan(-0.6))
        lead_notes.append(chord)
    inst = inst + _reverb(inst.mean(1), rate, 1.2, 0.12, rng)

    # vocal: phrases of notes from the current chord/scale, sung on random vowels
    f0 = np.zeros(n)
    amp = np.zeros(n)
    vowel_of = np.full(n, '', dtype='<U1')
    cons = np.zeros(n)
    pos = int(rng.uniform(0.5, 2.0) * beat)
    while pos < n:
        phrase = int(rng.integers(4, 9))
        for _ in range(phrase):
            ln = int(beat * rng.choice([0.5, 1, 1, 1.5, 2]))
            if pos + ln >= n:
                break
            chord = lead_notes[min(pos // bar, len(lead_notes) - 1)]
            m = (chord[rng.integers(0, 3)] if rng.random() < 0.7 else root + 12 + scale[rng.integers(0, 7)]) + 12
            f0[pos:pos + ln] = midi(m)
            env = np.minimum(np.arange(ln) / (0.03 * rate), 1) * np.minimum((ln - np.arange(ln)) / (0.05 * rate), 1)
            amp[pos:pos + ln] = env
            vowel_of[pos:pos + ln] = rng.choice(list(VOWELS))
            if rng.random() < 0.6:  # consonant at the onset
                cons[pos:pos + int(0.05 * rate)] = 1
            pos += ln
        pos += int(beat * rng.choice([1, 2, 3]))
    # glide between notes, vibrato that develops over each note
    voiced = f0 > 0
    f0 = np.where(voiced, f0, np.nan)
    idx = np.arange(n)
    f0 = np.interp(idx, idx[voiced], f0[voiced]) if voiced.any() else np.full(n, 220.0)
    glide = signal.lfilter([1 - 0.9985], [1, -0.9985], f0, zi=[f0[0] * 0.9985])[0]
    vib = 1 + 0.012 * np.sin(2 * np.pi * 5.5 * idx / rate) * np.clip(amp, 0, 1)
    f = glide * vib
    phase = 2 * np.pi * np.cumsum(f) / rate
    voice = np.zeros(n)
    for vw in VOWELS:
        sel = vowel_of == vw
        if not sel.any():
            continue
        part = np.zeros(n)
        for k in range(1, 40):
            fk = k * f
            g = _formant_gain(fk, vw) * (fk < 9000) / k ** 0.6
            part += g * np.sin(k * phase)
        w = signal.lfilter([0.02], [1, -0.98], sel.astype(float))  # smooth vowel changes
        voice += part * w
    voice *= signal.lfilter([0.01], [1, -0.99], amp)
    voice += signal.lfilter(*signal.butter(2, 4000 / (rate / 2), 'high'), rng.normal(0, 1, n)) * cons * 0.08
    voice /= np.sqrt(np.mean(voice ** 2)) + 1e-12
    vocal = np.stack([voice, voice], 1) * 0.5 + _reverb(voice, rate, 1.6, 0.25, rng)

    # levels: vocal 3 dB below the instrumental, peak headroom for mastering
    inst /= np.sqrt(np.mean(inst ** 2))
    vocal /= np.sqrt(np.mean(vocal ** 2))
    vocal *= 10 ** (-3 / 20)
    s = 0.25 / np.max(np.abs(vocal + inst))
    return vocal * s, inst * s
