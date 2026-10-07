# Evaluation harness

Every change to the separation is scored against the real Utagoe Rip 3.0 engine on
songs where the right answer is known, so improvements are measured, not judged by
ear.

```bash
pip install -r eval/requirements.txt          # numpy, scipy, soundfile; lame on the PATH for MP3
cmake -S . -B build -A x64 && cmake --build build --config Release
python eval/run.py                            # dev: 3 synthetic songs, all scenarios, all engines
python eval/run.py --set holdout41            # 20 synthetic songs nothing was tuned on
python eval/run.py --stems DIR --set test     # MUSDB18-HQ test songs 11-50
python eval/run.py --set holdout41 --metric leak  # another metric (scores are cached)
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
  held out. Failures they exposed were fixed while looking at them (listed under
  Results), so they are **not a clean holdout**.
* **fresh**: synthetic seeds 3000-3019, added later; three of them (3000-3002) were
  used to diagnose a fast-drift problem.
* **final**: synthetic seeds 4000-4019, added for 4.0's final numbers. Nothing was
  tuned on them; they were later used to compare against another implementation.
* **final41**: synthetic seeds 5000-5019, added for 4.1's numbers. Nothing was tuned on
  them, but they were scored after each of 4.1's last revisions.
* **holdout41**: synthetic seeds 6000-6019, added after those revisions and scored
  once, with the final code. Nothing was tuned on them.

## Engines

| Engine | What it is |
|---|---|
| `3.0` | the shipped engine (`tests/engine_test`, default settings) |
| `ceiling` | MP3 only: perfect subtraction of the coded karaoke |
| `proto-scalar` | 3.0's method in the prototype (one level, 3.0's rule, one global offset); checks the prototype is a fair stand-in |
| `proto-scalar-kill` | 3.0 plus one change: the keep-or-delete test uses the level-scaled instrumental |
| `proto-eq` | per-band EQ matching, one global offset |
| `proto-eq-align` | plus frame-by-frame alignment |
| `v4` | plus automatic level tracking: Utagoe Rip 4.0 |
| `v41` | plus the EQ by Huber-weighted least squares and the soft decision: Utagoe Rip 4.1 |
| `v4-cpp` | the C++ port of `v41` (`src/engine/v4.cpp` through `tests/v4_cli`), what the program runs |
| `v40-cpp` | the same port run as 4.0 (`v4_cli --hard`) |

`proto.py` also keeps variants that were tried and not adopted (`proto-eq-ls1`,
`proto-eq-align-lvl`, `proto-eq-align-auto`, `v4-cv`), for reproducibility. `v4`
(4.0) and `v41` share everything but the EQ estimate, the decision and the constant-lag test.

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
local-linear smoother (1.5 s). In 4.1 a line whose drift adds up to less than 0.1
samples over the song, or to less than 3 standard errors of its slope, is taken as a
constant lag. Identical digital releases have no drift, and the slope's measurement
noise alone left errors of a few hundredths of a sample that limited an exact
subtraction to about 47 dB (52 dB on the dev songs without them); an EQ difference
biases each window's fraction by an amount that follows the music, which can fake a
drift of a few tenths of a sample. A real drift taken for none is caught by the
per-frame refinement below once its median correction reaches 0.75 samples, which for
a steady drift means about 3 samples over the song, and then followed. Before fitting, windows more than 2 samples from the
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

**EQ matching.** A complex gain per frequency bin, smoothed over 1/3 octave. 4.1
starts from least squares over all cells and refines it twice with Huber weights: per
bin, cells whose residual `|O - H K|` is more than 1.5 times the bin's median residual
(the vocal) count proportionally less. The vocal is uncorrelated with the instrumental,
so least squares is unbiased; the weights only keep it from adding noise. (4.0 took a
second pass over the cells the first said the instrumental dominates, with `|H|` from
their power ratio; that selection biases the estimate slightly, which capped how well
an exact match could subtract.)

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

**Soft decision (4.1).** 3.0 keeps or deletes each time-frequency cell: it deletes a
cell when the instrumental explains it (`cap * |K| > |O|` and the phases agree). That
throws away the vocal in every cell the instrumental dominates, and caps the result
near 25-28 dB even when the subtraction itself is exact. 4.1 keeps
`max(0, 1 - s rho |I|^2 / |O - I|^2)` of each cell's `O - I`, where `I = H K` is the
instrumental as estimated in the mix and `rho` is the model's error in that 1/3-octave
band: a quantile of `|O - I|^2 / |I|^2` over the cells the instrumental dominates. An
exact model (`rho` near 0) subtracts and keeps everything; an imprecise one fades out
the cells where the residual is mostly model error. Those cells still hold some
vocal, so the quantile is low (0.10) where the model can be exact (no level tracking,
a lag that is a straight line, no lossy coding) and the median where the releases'
dynamics differ, the lag wobbles, or either file shows a lossy coder's low-pass (a drop
of more than 20 dB within 1 kHz between 11 and 20.5 kHz): coding noise differs between
the two files, so the model cannot be exact there either. On the dev songs that rule
picked the better of the two in every scenario. Two MUSDB songs come from band-limited
sources and are treated as lossy in every scenario, which costs them in the cases where
both files are identical (in practice an album and a karaoke track are coded
separately). `s` is the Extractable Level setting over its
default (1.2), doubled in Extraction Priority mode. 4.0 used 3.0's rule against `I`.

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

4.1 and 4.0 here are the C++ separation as the program runs it (`v4-cpp`) and as 4.0
ran it (`v40-cpp`); the Python prototypes give the same output to -55 dB or better.
Median SDR in dB, higher is better; the difference is per song, with a 95 % bootstrap
confidence interval and the number of songs 4.1 beats 3.0 on. The last column is the
same difference for leak (positive: 4.1 leaks less).

**Holdout synthetic songs** (`holdout41`: 20, added last and scored once, nothing tuned on them):

| Scenario | 3.0 | 4.0 | 4.1 | 4.1 − 3.0 [95 % CI] | 4.1 better | leak, 4.1 better by |
|---|---:|---:|---:|---:|---:|---:|
| `clean` | 28.32 | 28.39 | 58.35 | +30.03 [+29.15, +30.94] | 20/20 | +19.85 [+18.12, +21.23] |
| `level` | 27.95 | 29.91 | 57.47 | +29.52 [+28.76, +30.33] | 20/20 | +19.04 [+17.43, +20.33] |
| `inverted` | 28.31 | 28.38 | 51.90 | +23.59 [+22.92, +24.34] | 20/20 | +12.90 [+11.78, +14.00] |
| `album_eq` | 3.97 | 27.02 | 32.25 | +28.28 [+27.45, +28.95] | 20/20 | +27.84 [+26.71, +28.67] |
| `album_loud` | -3.23 | 10.08 | 11.35 | +14.58 [+14.03, +15.13] | 20/20 | +13.86 [+12.66, +15.41] |
| `both_loud` | 22.34 | 23.40 | 23.75 | +1.41 [+0.97, +1.89] | 19/20 | +1.67 [+0.44, +2.89] |
| `both_diff` | 8.23 | 23.10 | 23.22 | +14.99 [+14.36, +15.64] | 20/20 | +12.85 [+11.31, +14.90] |
| `offset_frac` | 26.52 | 28.39 | 51.33 | +24.81 [+23.98, +25.75] | 20/20 | +12.73 [+10.98, +14.48] |
| `drift` | 27.03 | 28.38 | 41.25 | +14.22 [+11.64, +16.85] | 20/20 | +3.88 [+0.59, +7.12] |
| `drift_fast` | 20.55 | 27.64 | 32.82 | +12.27 [+11.21, +13.22] | 20/20 | +9.40 [+7.88, +10.69] |
| `wow` | 19.28 | 25.46 | 23.35 | +4.08 [+3.29, +4.84] | 20/20 | +6.67 [+4.88, +8.12] |
| `mp3` | 20.18 | 19.67 | 19.64 | -0.55 [-0.64, -0.46] | 0/20 | -1.22 [-1.40, -1.05] |
| `everything` | 7.13 | 15.74 | 16.63 | +9.50 [+9.17, +9.85] | 20/20 | +11.46 [+10.96, +11.95] |

The `final41`, `final` and `fresh` songs give the same picture (every 4.1 value within
1.3 dB of these).

**MUSDB18-HQ test songs 11-50** (30 s excerpts; 39 songs, see below):

| Scenario | 3.0 | 4.0 | 4.1 | 4.1 − 3.0 [95 % CI] | 4.1 better | leak, 4.1 better by |
|---|---:|---:|---:|---:|---:|---:|
| `clean` | 23.67 | 23.78 | 49.71 | +26.04 [+22.82, +29.08] | 39/39 | +11.33 [+9.20, +13.48] |
| `level` | 23.51 | 25.30 | 49.64 | +26.13 [+23.08, +29.03] | 39/39 | +10.15 [+8.46, +11.74] |
| `inverted` | 23.67 | 23.78 | 47.00 | +23.34 [+20.70, +25.73] | 39/39 | +6.67 [+5.30, +7.94] |
| `album_eq` | 5.45 | 22.62 | 31.06 | +25.61 [+24.25, +26.78] | 39/39 | +28.77 [+27.52, +30.11] |
| `album_loud` | -4.37 | 11.65 | 12.73 | +17.11 [+16.08, +18.08] | 39/39 | +18.79 [+17.27, +20.31] |
| `both_loud` | 17.39 | 19.25 | 20.23 | +2.84 [+1.75, +4.23] | 34/39 | +0.76 [-1.84, +3.93] |
| `both_diff` | 7.09 | 17.76 | 18.31 | +11.22 [+10.36, +12.00] | 39/39 | +16.36 [+14.90, +17.86] |
| `offset_frac` | 22.26 | 23.75 | 48.74 | +26.48 [+23.38, +29.37] | 39/39 | +9.52 [+7.78, +11.21] |
| `drift` | 22.47 | 23.76 | 45.00 | +22.53 [+20.02, +24.88] | 39/39 | +14.08 [+11.69, +16.40] |
| `drift_fast` | 19.13 | 23.22 | 42.78 | +23.65 [+20.63, +26.37] | 38/39 | +23.21 [+20.21, +25.97] |
| `wow` | 18.08 | 19.96 | 21.67 | +3.59 [+2.29, +4.72] | 33/39 | +7.51 [+5.36, +9.69] |
| `mp3` | 19.54 | 19.45 | 19.87 | +0.33 [+0.04, +0.62] | 27/39 | -2.41 [-3.20, -1.58] |
| `everything` | 6.19 | 14.81 | 15.07 | +8.87 [+8.14, +9.67] | 39/39 | +14.08 [+12.96, +15.30] |

Songs 11-50 are 40 songs; *Skelpolu - Resurrection* has no vocal in its excerpt, so its
median SDR is undefined (it still counts for leak, where 36 songs have pauses).

**Whole songs.** The tables use 30 s excerpts. On six whole songs (MUSDB18-HQ test
songs 1-3 and 11-13, `--seconds 600 --start 0`), 4.1's lead over 3.0 per scenario was:
`clean` +32.7 / +35.1 dB (songs 1-3 / 11-13), `drift` +27.9 / +30.3, `drift_fast` +30.6
/ +33.6, `both_loud` +3.8 / +2.3, `both_diff` +12.4 / +10.7, `wow` +8.4 / +3.2,
`everything` +7.6 / +9.3; 4.1 was better on every song in each of these. Against 4.0 it
was 1.2-1.7 dB ahead on songs 1-3 and 0.2-0.5 dB behind on songs 11-13 where the
releases' dynamics differ (`both_loud`, `both_diff`).

**Read with care:**

* **The MUSDB test songs are not a clean holdout.** Test runs exposed failures
  (picking a repeat of the instrumental a few bars away, getting the polarity wrong, a
  sawtooth in the drift curve, the album EQ's phase misread as timing, level tracking
  switching on when it should not, an outlier at the start of the tracked curve, level
  tracking blind in the vocal's pauses, unreliable windows in a near-silent passage)
  that were fixed while looking at those songs; where there was a choice between
  fixes, it was made on the dev songs. 4.1's settings were chosen on the dev songs; one
  4.1 fix (the drift test) came from a test song (*Little Chicago's Finest*) and the
  lossy-coding gate from the test songs' MP3 rows; both were adopted after the dev songs
  confirmed them. The fixes are general, but the MUSDB
  numbers are optimistic by an unknown amount. The final synthetic songs are clean but
  synthetic. A clean real-music check
  would need songs nothing was tuned on, such as the MUSDB18-HQ training set (the same
  22.7 GB download).
* **Where 4.1 is not better.** Wow can still defeat the tracking: with 2 s windows
  against a 1.8 s wobble, the coarse curve can lose a stretch, and on one MUSDB song
  (*Mu - Too Bright*) 4.1 is 9 dB below 3.0 on `wow` (three others lose 1-3 dB). On the
  synthetic songs 4.1 is about 2 dB below 4.0 on `wow`: where the lag wobbles, the soft
  decision keeps more of what the delete rule removed. With MP3 files, 4.1 leaves a
  little more coding noise in the vocal's pauses than 3.0 (leak 1-2.5 dB higher) and
  scores 0.5 dB below it on the synthetic songs; the `mp3` rows are approximate
  (above). Keeping more of every cell also means that where the model is imperfect,
  4.1 leaves a little more instrumental in the pauses than 4.0 (`album_eq`: about 10 dB
  more leak than 4.0, still 29 dB less than 3.0).
* **Speed.** The separation reads both files several times (lag search, tracking, the
  EQ's passes, level tracking, output), using all processor cores: a 3:20 song takes
  19 s on a 16-thread desktop, against 6 s for 3.0.
