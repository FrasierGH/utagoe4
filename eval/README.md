# Evaluation harness

Every change to the separation is scored against the real Utagoe Rip 3.0 engine on
songs where the right answer is known, so improvements are measured, not judged by
ear.

```bash
pip install -r eval/requirements.txt          # numpy, scipy, soundfile; lame on the PATH for MP3
cmake -S . -B build -A x64 && cmake --build build --config Release
python eval/run.py                            # dev: 3 synthetic songs, all scenarios, all engines
python eval/run.py --set final                # 20 synthetic songs nothing was tuned on
python eval/run.py --stems DIR --set test     # MUSDB18-HQ test songs 11-50
python eval/run.py --set final --metric leak  # another metric (scores are cached)
```

Long runs can be split with `--shard i/n` (every n-th song); `--report` merges the
saved results of all shards. Cases and scores are cached in `eval/work/` (ignored by
git) under a hash of the code and settings that made them, so editing a scenario or
an engine recomputes what it affects; `--prune` deletes what older versions left.
Engine outputs are only kept with `--keep-audio`.

## Pieces

| File | What it does |
|---|---|
| `synth.py` | synthetic songs: a vocal stem (formant voice with vibrato, glides, consonants, stereo reverb) and an instrumental stem (drums, bass, pads, lead) |
| `degrade.py` | scenarios: how an album and its karaoke release differ |
| `proto.py` | the Python prototype of every engine; `ENGINES` names the configurations |
| `metrics.py` | median SDR (headline), whole-file SDR, SI-SDR, leak |
| `run.py` | builds the cases, runs the engines, prints tables with per-song differences and 95 % bootstrap confidence intervals |
| `prepare_musdb.py` | unpacks the MUSDB18-HQ test set |

## Scenarios

Each returns a mix, a karaoke file and the target: the vocal as it sounds inside
the mix.

| Scenario | Album vs karaoke |
|---|---|
| `clean` | identical instrumental, aligned (the ideal case) |
| `level` | karaoke at 0.8x the level |
| `inverted` | karaoke with inverted polarity |
| `album_eq` | album EQ'd, karaoke raw; no dynamics processing |
| `album_loud` | album loudly mastered (EQ, compressor, limiter), karaoke raw |
| `both_loud` | both through the same loud chain, each with its own gain envelope (the usual commercial case) |
| `both_diff` | karaoke mastered by a different chain (a remaster) |
| `offset_frac` | karaoke 1234.37 samples late |
| `drift` | karaoke plays 30 ppm slow (40 samples behind after 30 s) |
| `drift_fast` | 300 ppm (400 samples after 30 s) |
| `wow` | vinyl/tape speed wobble: +-6 samples at 0.55 Hz (a 33 rpm record) plus a slow +-4 |
| `mp3` | both through 192 kbps MP3 |
| `everything` | remaster + level + offset + drift + MP3 |

Mastering is an EQ followed by gain envelopes computed on the whole release, so the
vocal inside a mastered mix is exactly `gain(t) * EQ(vocal)`. The chains are
calibrated to the signal (compressor threshold relative to its RMS, limiter driven
by a set amount), so they act on every song: the gain moves by 7-11 dB (1st to 99th
percentile) and the limiter holds the peaks at its ceiling. Each case records this
in `stats.json`.

**MP3 targets are approximate.** Coding noise is not separable, so the `mp3` and
`everything` targets are the uncoded vocal. An engine can score *above* perfect
subtraction of the coded karaoke (the `ceiling` engine) by deleting bins where coding
noise dominates, so on these rows a higher score does not mean a better instrumental
estimate.

## Metrics

All ignore the first and last second (start-up and tail effects in every engine).

* **median SDR**: median over 1 s windows where the vocal sings. The headline number.
* **SDR**, **SI-SDR**: whole file; SI-SDR ignores an overall gain error.
* **leak**: the error where the vocal is silent, relative to the instrumental there
  (lower is better). Undefined when the vocal never pauses. An engine that outputs
  silence leaks nothing, so read it only next to an SDR.

## Song sets

* **dev**: synthetic seeds 1000-1002 and MUSDB18-HQ test songs 1-10. Everything was
  developed and tuned on these.
* **test**: synthetic seeds 2000-2009 and MUSDB18-HQ test songs 11-50, meant to be
  held out. Their runs exposed failures (below) that were then fixed while looking at
  the failing cases, so they are **no longer held out**.
* **fresh**: synthetic seeds 3000-3019, added after those fixes. Three of them
  (3000-3002) were later looked at to find out why v4 lost on fast drift, which led to
  resampling the karaoke (the threshold was then chosen on dev songs).
* **final**: synthetic seeds 4000-4019, added last, for the final numbers. Nothing was
  tuned on them; only their summary table was looked at, after each of the last four
  rounds of fixes (which came from MUSDB songs and from a code review).

## Engines

| Engine | What it is |
|---|---|
| `3.0` | the shipped engine (`tests/engine_test`, default settings) |
| `ceiling` | MP3 only: perfect subtraction of the coded karaoke |
| `proto-scalar` | 3.0's method in the prototype (one level, 3.0's rule, one global offset); checks the prototype is a fair stand-in |
| `proto-scalar-kill` | 3.0 plus one change: the keep-or-delete test uses the level-scaled instrumental |
| `proto-eq` | per-band EQ matching, one global offset |
| `proto-eq-align` | plus frame-by-frame alignment |
| `v4` | plus automatic level tracking: the Utagoe Rip 4 separation |
| `v4-cpp` | the C++ port of `v4` (`src/engine/v4.cpp` through `tests/v4_cli`), what the program runs |

`proto.py` also keeps variants that were tried and not adopted (`proto-eq-ls1`,
`proto-eq-align-lvl`, `proto-eq-align-auto`, `v4-cv`), for reproducibility.

## How v4 works

**Lag and polarity.** Candidates are the strongest GCC-PHAT peaks of the whole file
(searched within +-30 s; songs over a minute use a one-minute excerpt from the
middle), plus the best peaks of 8 s windows spread over the song (within +-10 s):
drift smears the whole-file peak and loop-based music adds rival peaks a few bars
away, but the true lag tends to win in some windows. Lags within 512 samples count as
one. Each candidate is tracked through a one-minute excerpt (as below, ignoring
polarity) and scored by a trial subtraction in the 100 Hz-8 kHz band along its own
curve, where repeats differ more than they do below 1 kHz. The winner gives the lag
and, from the sign of its trial gain, the polarity; candidates within 5 % of the best
score (a song that repeats itself) are told apart by how much of the two files
overlaps at their lag. Files under 8 s are processed as 3.0 does (in the program, not
in `v4_cli`): below that the estimates have too little to go on. Band edges stay
below Nyquist at low sample rates.

**Tracking.** Windows of 2 s every 1 s, starting where the lag was measured and
working outwards; each searches around the previous confident estimate, so drift is
followed. The integer part comes from GCC-PHAT (either polarity), the fraction from the
slope of the cross-spectrum phase. A second pass re-measures after reading the karaoke
along the first curve (fast drift smears the first pass), and the final curve is fitted
to its absolute lags: a straight line if it fits within 0.2 samples, otherwise a
local-linear smoother (1.5 s). Before fitting, windows more than 2 samples from the
running median of their neighbours are dropped; the first and last two, where a running
median cannot judge, must lie within 8 samples of the line through their four inner
neighbours. Windows whose correlation peak is under half the median peak (the
instrumental nearly silent, as in an a cappella passage) are not fitted either; the
curve is carried over them from the reliable windows around.

**Per-frame refinement.** A wobble faster than the windows (a 33 rpm record repeats
every 1.8 s) averages out in them and can pass for a straight line, so a per-frame
refinement always runs once: the phase slope of each STFT frame's cross-spectrum
(100 Hz-2 kHz), with bins weighted by how much the instrumental dominates them,
smoothed over a few frames. It compares the mix with the karaoke *through the phase
of a first EQ estimate* (without its linear part, which is a pure delay), because an EQ
difference has a phase response (a low shelf delays the low band) that would otherwise
read as timing; the estimate is redone before the second pass, once most of a wobble is
gone. If the curve was a line and the median correction is under 0.75 samples, the line is
kept; otherwise the refinement is applied and repeated up to 6 kHz. Accuracy on known
curves: 0.01-0.04 samples for offsets and drift up to 300 ppm, wow 0.14 typical (a few
frames up to ~2.4).

**Reading the karaoke along the curve.** While the lag holds still over a frame (8192
samples), each frame is cut at its integer lag and the fraction applied as a linear
phase, which is exact. When the lag moves by more than 0.6 samples across a frame
(drift faster than about 70 ppm, or wow) a frame cut at one lag is off towards its
edges, which decorrelates the highs and biases the EQ estimate low there; the karaoke is
then resampled along the curve (128-tap Kaiser-windowed sinc) instead. On the
benchmark, resampling raised v4's lead on fast drift and wow by 1.6-2 dB on average on
the MUSDB songs and by 6-9 dB on the synthetic ones.

**EQ matching.** A complex gain per frequency bin, smoothed over 1/3 octave, in two
passes: the second uses only the cells the first says the instrumental dominates,
with `|H|` from the power ratio of those cells (selecting cells by `|O|` truncates
`O` and biases least squares low; the power ratio mostly undoes that).

**Level tracking (auto).** A per-frame gain correction for releases whose dynamics
differ, smoothed over ~50 ms, in three steps: least squares over all cells (the vocal
is uncorrelated with the instrumental, so this is unbiased even where the estimate is
far off), then over the cells the corrected estimate says the instrumental dominates,
then the power ratio of those cells. The first step matters in the vocal's pauses: a
compressor driven by the vocal works less there, the album's instrumental comes up by
several dB, and an estimator that only looks at cells the current estimate already
explains finds none. It is applied only when it holds up out of sample: estimated on
the even frequency bins, it must cut the residual on the odd bins by more than 13 %.
On the dev songs it cut it by at most 3.5 % without a dynamics difference and by at
least 15.5 % with one.

**Kill rule.** As 3.0 (`cap * |I| > |O|` and, in quality mode, the phases agree), but
against the instrumental as estimated in the mix, `I = H K`, not 3.0's raw `|K|`.

## Real songs

`--stems DIR` takes a folder of song folders, each with `vocals.wav` and either
`accompaniment.wav` or `drums.wav`, `bass.wav` and `other.wav` (the MUSDB18-HQ layout),
44.1 kHz. A 30 s excerpt starting at `--start` (30 s) is used (`--seconds` and
`--start` change it); `--limit N` takes the first N songs of the set. Datasets are not
part of the repository.

MUSDB18-HQ ([Zenodo](https://zenodo.org/records/3338373), 22.7 GB zip, open access,
non-commercial use only) has 50 test songs in that layout:

```bash
python eval/prepare_musdb.py musdb18hq.zip D:/datasets/musdb18hq --md5 12d4f2ecd55245a4688754dd76363103 --delete-zip
python eval/run.py --stems D:/datasets/musdb18hq/test --set test
```

## Results

v4 here is `v4-cpp`, what the program runs (the Python `v4` gives the same output to
-60 dB or better). Median SDR in dB, higher is better; the difference is per song, with
a 95 % bootstrap confidence interval and the number of songs v4 wins. The last column
is the same difference for leak (positive: v4 leaks less).

**Final synthetic songs** (20, added last, nothing tuned on them):

| Scenario | 3.0 | v4 | v4 − 3.0 [95 % CI] | v4 better | leak, v4 better by |
|---|---:|---:|---:|---:|---:|
| `clean` | 27.99 | 28.05 | +0.06 [-0.02, +0.15] | 14/20 | +0.18 [+0.10, +0.27] |
| `level` | 27.74 | 29.61 | +1.86 [+1.68, +2.02] | 20/20 | +0.60 [+0.15, +0.94] |
| `inverted` | 27.97 | 28.04 | +0.07 [-0.01, +0.16] | 14/20 | +0.25 [+0.15, +0.36] |
| `album_eq` | 3.76 | 26.58 | +22.82 [+22.13, +23.43] | 20/20 | +36.17 [+34.93, +37.56] |
| `album_loud` | -3.32 | 9.47 | +12.79 [+11.58, +13.88] | 20/20 | +12.20 [+10.91, +13.42] |
| `both_loud` | 22.40 | 23.32 | +0.92 [+0.67, +1.19] | 19/20 | -1.01 [-2.46, +0.23] |
| `both_diff` | 8.02 | 22.89 | +14.86 [+14.31, +15.45] | 20/20 | +13.65 [+12.74, +14.74] |
| `offset_frac` | 26.32 | 28.04 | +1.72 [+1.43, +2.02] | 20/20 | +1.33 [+0.74, +2.02] |
| `drift` | 26.60 | 28.04 | +1.44 [+1.16, +1.77] | 20/20 | +2.78 [+2.13, +3.55] |
| `drift_fast` | 20.06 | 27.19 | +7.13 [+6.70, +7.55] | 20/20 | +11.90 [+11.16, +12.59] |
| `wow` | 18.33 | 25.14 | +6.81 [+6.37, +7.23] | 20/20 | +9.57 [+8.92, +10.30] |
| `mp3` | 20.11 | 19.64 | -0.47 [-0.54, -0.41] | 0/20 | -1.79 [-1.91, -1.67] |
| `everything` | 6.87 | 15.62 | +8.75 [+8.47, +9.03] | 20/20 | +10.07 [+9.74, +10.40] |

The `fresh` songs give the same picture (every row within 0.7 dB of these).

**MUSDB18-HQ test songs 11-50** (30 s excerpts; 39 songs, see below):

| Scenario | 3.0 | v4 | v4 − 3.0 [95 % CI] | v4 better | leak, v4 better by |
|---|---:|---:|---:|---:|---:|
| `clean` | 23.67 | 23.78 | +0.11 [-0.10, +0.32] | 25/39 | +0.14 [+0.03, +0.25] |
| `level` | 23.51 | 25.30 | +1.79 [+1.53, +2.07] | 37/39 | +1.22 [+0.64, +2.07] |
| `inverted` | 23.67 | 23.78 | +0.11 [-0.09, +0.32] | 26/39 | +0.42 [+0.13, +0.90] |
| `album_eq` | 5.45 | 22.62 | +17.18 [+16.18, +18.17] | 39/39 | +38.74 [+36.50, +41.05] |
| `album_loud` | -4.37 | 11.65 | +16.03 [+14.83, +17.12] | 39/39 | +17.29 [+15.41, +19.07] |
| `both_loud` | 17.39 | 19.25 | +1.86 [+0.96, +3.16] | 35/39 | +0.28 [-1.74, +2.58] |
| `both_diff` | 7.09 | 17.76 | +10.67 [+9.87, +11.46] | 39/39 | +17.12 [+15.25, +19.05] |
| `offset_frac` | 22.26 | 23.75 | +1.49 [+0.81, +2.60] | 37/39 | +0.32 [-0.89, +1.71] |
| `drift` | 22.47 | 23.76 | +1.29 [+0.73, +2.18] | 37/39 | +9.19 [+6.96, +11.57] |
| `drift_fast` | 19.13 | 23.22 | +4.09 [+3.59, +4.58] | 39/39 | +19.35 [+16.57, +22.05] |
| `wow` | 18.08 | 19.96 | +1.88 [+0.80, +2.85] | 30/39 | +9.05 [+6.18, +12.02] |
| `mp3` | 19.54 | 19.45 | -0.10 [-0.18, -0.02] | 12/39 | -1.36 [-1.76, -0.93] |
| `everything` | 6.19 | 14.81 | +8.62 [+7.83, +9.45] | 39/39 | +13.01 [+11.94, +14.18] |

Songs 11-50 are 40 songs; *Skelpolu - Resurrection* has no vocal in its excerpt, so its
median SDR is undefined (it still counts for leak, where 36 songs have pauses).

**Whole songs.** The tables use 30 s excerpts. On six whole songs (MUSDB18-HQ test
songs 1-3 and 11-13, `--seconds 600 --start 0`), v4's lead over 3.0 per scenario was:
`both_loud` +2.1 / +2.5 dB (songs 1-3 / 11-13), `both_diff` +11.2 / +11.2, `drift`
+1.7 / +0.9, `drift_fast` +6.9 / +4.8, `wow` +6.6 / +3.4, `everything` +7.0 / +9.1; v4
was better on every song in each of these. `clean` was a tie (+0.04 / -0.14).

**Read with care:**

* **The MUSDB test songs are not a clean holdout.** Test runs exposed failures
  (picking a repeat of the instrumental a few bars away, getting the polarity wrong, a
  sawtooth in the drift curve, the album EQ's phase misread as timing, level tracking
  switching on when it should not, an outlier at the start of the tracked curve, level
  tracking blind in the vocal's pauses, unreliable windows in a near-silent passage)
  that were fixed while looking at those songs; where there was a choice between
  fixes, it was made on the dev songs. The fixes are general, but the MUSDB numbers
  are optimistic by an unknown amount. The final synthetic songs are clean but synthetic. A clean real-music check
  would need songs nothing was tuned on, such as the MUSDB18-HQ training set (the same
  22.7 GB download).
* **Where v4 is not better.** Wow can still defeat the tracking: with 2 s windows
  against a 1.8 s wobble, the coarse curve can lose a stretch, and on one MUSDB song
  (*Mu - Too Bright*) v4 is 11 dB below 3.0 on `wow` (four other songs lose 2-3 dB);
  on average v4 still wins `wow` by 1.9 dB there. With identical instrumentals
  (`clean`) v4 ties 3.0: matching EQ per band costs a little against one level
  (`proto-scalar` is 0.14 dB ahead of v4 on the MUSDB songs), and on one song (*Nerve 9*)
  v4 is 2.4 dB below 3.0. Leak on `both_loud` is about even. The `mp3` rows are
  approximate (above).
* **Speed.** v4 reads both files several times (lag search, tracking, two EQ passes,
  level tracking, output), using all processor cores: a 3:20 song takes 18 s on a
  16-thread desktop, against 6 s for 3.0.

