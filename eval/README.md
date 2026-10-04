# Evaluation harness

Every change to the separation is scored against the real Utagoe Rip 3.0 engine on
songs where the right answer is known, so improvements are measured, not judged by
ear.

```bash
pip install -r eval/requirements.txt          # numpy, scipy, soundfile
cmake -S . -B build -A x64 && cmake --build build --config Release --target engine_test
python eval/run.py                            # 3 synthetic songs x all scenarios x all engines
python eval/run.py --metric bleed             # same results, another metric (all cached)
```

MP3 scenarios need `lame` on the PATH. Everything is cached in `eval/work/` (ignored by
git); delete it to rebuild.

## Pieces

| File | What it does |
|---|---|
| `synth.py` | synthetic songs: a vocal stem (formant voice with vibrato, glides, consonants, stereo reverb) and an instrumental stem (drums, bass, pads, lead) |
| `degrade.py` | scenarios: how album and karaoke releases differ (level, mastering EQ, compression and limiting, sub-sample offset, clock drift, MP3) |
| `proto.py` | the prototype separator, with the improvements as switches |
| `metrics.py` | median SDR over 1 s windows where the vocal sings (the headline number), whole-file SDR, SI-SDR, and bleed: instrumental left where the vocal is silent |
| `run.py` | builds the cases, runs every engine, prints a table, writes `eval/work/results_*.csv` |

In the mastering scenarios the mix is `master(vocal + instrumental)` while the karaoke
file is the plain instrumental. The master is an EQ followed by gain envelopes, so the
vocal as heard in the mix is exactly `gain(t) * EQ(vocal)`, and that is the target.

Engines: `3.0` is the shipped engine (`tests/engine_test`, default settings).
`proto-scalar` is the prototype doing what 3.0 does (one level, 3.0's decision rule)
and should score like `3.0`; it checks that the prototype is a fair stand-in. Each
improvement gets its own engine.

## Real songs

Synthetic songs are for development; changes should be confirmed on real multitracks.
`--stems DIR` takes a folder of song folders, each with `vocals.wav` and either
`accompaniment.wav` or `drums.wav`, `bass.wav` and `other.wav` (the MUSDB18-HQ layout),
44.1 kHz. A 30 s excerpt starting at `--start` (30 s) is used; `--limit N` takes the
first N songs. Datasets are not part of the repository.

MUSDB18-HQ ([Zenodo](https://zenodo.org/records/3338373), 22.7 GB zip, open access,
non-commercial use only) has 50 test songs in that layout:

```bash
python eval/prepare_musdb.py musdb18hq.zip D:/datasets/musdb18hq --md5 12d4f2ecd55245a4688754dd76363103 --delete-zip
python eval/run.py --stems D:/datasets/musdb18hq/test
```

## Results so far

Median SDR in dB (higher is better), mean of 3 synthetic 30 s songs:

| scenario | 3.0 | proto-scalar | proto-eq | proto-eq-align |
|---|---:|---:|---:|---:|
| clean | 27.23 | 27.32 | 27.31 | 27.30 |
| level | 27.25 | 27.32 | 28.99 | 28.99 |
| master_eq | 3.46 | 3.92 | **26.56** | **26.52** |
| master_full | 1.18 | 1.17 | **25.92** | **25.92** |
| offset_frac | 25.62 | 25.69 | 27.04 | 27.06 |
| drift | 25.64 | -1.20 | 0.23 | **27.08** |
| mp3 | 19.24 | 19.31 | 18.81 | 18.80 |
| everything | 0.17 | -2.98 | 2.16 | **14.50** |
| mean | 16.22 | 12.57 | 19.63 | **24.52** |

Bleed (instrumental left where the vocal is silent, dB, lower is better): 3.0 leaves
-11 / -8 dB on the mastered scenarios and -7 dB on `everything`; `proto-eq-align` gets
-43 / -41 and -19 dB.

**1. Per-band EQ matching (`proto-eq`).** Instead of one level, estimate a complex
gain per frequency bin and subtract `H K`. Findings:

* It is what makes mastered albums work at all: 3.0 drops to 1-3 dB there.
* The phase of `H` also corrects a residual sub-sample offset, hence the gain on
  `offset_frac` and `level`.
* Estimated from every cell, the vocal adds noise to `H`: at 1/6 octave that cost
  2 dB on `clean`. Two passes fix it: the second only uses the cells the first says
  the instrumental dominates. Smoothing is 1/3 octave (2/3 and 1 octave scored the
  same here; 1/3 keeps narrow EQ moves on real masters).
* `|H|` comes from the power ratio of those cells, `sqrt(sum |O|^2 / sum |K|^2)`, with
  the phase of the cross-spectrum. Least squares (`sum O conj(K) / sum |K|^2`) is
  biased low when the karaoke carries noise of its own (MP3 coding noise above
  4 kHz): the power ratio gained 0.6 dB on `mp3` and 0.2-0.4 dB everywhere else.
* A per-frame level correction on top (for compression) made every scenario
  0.5-1 dB worse, so it is off.

**2. Time-varying, sub-sample alignment (`proto-eq-align`).** The lag is measured in
4 s windows every 2 s: GCC-PHAT for the integer part, the slope of the
cross-spectrum phase (100 Hz - 8 kHz) for the fraction. A weighted straight line is
fitted with outliers dropped (clock drift), falling back to a smoothed curve when a
line doesn't fit. A second pass re-measures after aligning, because drift within a
window (5 samples at 30 ppm) biases the first. Measured accuracy: 0.07 samples on a
constant 1234.37-sample offset; drift is tracked to the same accuracy. The
karaoke isn't resampled for the subtraction: each STFT frame is cut at its own
integer lag and the fraction is applied as a linear phase (error -77 dB up to
20 kHz). Result: `drift` goes from failing to 27.1 dB (3.0: 25.6), `everything` from
2.2 to 14.5 dB (3.0: 0.2).

**Open:**

* `mp3` is 0.4 dB below 3.0, all of it above 10 kHz.
* Below 200 Hz every engine, 3.0 included, leaves error above the vocal's own energy
  there (bass and kick leaking into the vocal).
* Everything is still synthetic; next is confirming on MUSDB18-HQ.
