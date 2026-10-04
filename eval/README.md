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

| scenario | 3.0 | proto-scalar | proto-eq |
|---|---:|---:|---:|
| clean | 27.23 | 27.32 | 27.07 |
| level | 27.25 | 27.32 | 28.61 |
| master_eq | 3.46 | 3.92 | **26.38** |
| master_full | 1.18 | 1.17 | **25.56** |
| offset_frac | 25.62 | 25.69 | 26.78 |
| drift | 25.64 | -1.20 | 0.33 |
| mp3 | 19.24 | 19.31 | 18.23 |
| everything | 0.17 | -2.98 | 2.09 |

Bleed where the vocal is silent: 3.0 leaves the instrumental at -11 dB (EQ'd master)
and -8 dB (full master); `proto-eq` gets -43 and -41 dB.

**1. Per-band EQ matching (`proto-eq`).** Instead of one level, estimate a complex
gain per frequency bin, `H(f) = sum O conj(K) / sum |K|^2`, smoothed over 1/3 octave,
and subtract `H K`. Findings:

* It is what makes mastered albums work at all: 3.0 drops to 1-3 dB there.
* It also corrects a residual sub-sample offset (a linear phase), hence the gain on
  `offset_frac` and `level`.
* Estimated from every cell, the vocal adds noise to `H`: 1/6 octave cost 2 dB on
  `clean`. Two passes fix this. The second estimate only uses the cells the first says
  the instrumental dominates. With 1/3 octave that is within 0.25 dB of 3.0 on `clean`.
  Wider smoothing (2/3, 1 octave) scored the same on these songs, but 1/3 is kept for
  narrow EQ moves on real masters.
* A per-frame level correction on top (for compression) made every scenario
  0.5-1 dB worse, so it is off.
* MP3 is about 1 dB worse than 3.0. Not yet understood.

**Next: alignment.** The prototype aligns the whole file once, so it fails on
`drift` (and `everything`), which 3.0 handles by re-searching the offset per block.
Time-varying (and sub-sample) alignment is the next improvement; until then the
`drift` and `everything` rows say nothing about EQ matching.
