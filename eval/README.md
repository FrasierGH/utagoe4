# Evaluation harness

Every change to the separation is scored against the real Utagoe Rip 3.0 engine on
songs where the right answer is known, so improvements are measured, not judged by
ear.

```bash
pip install -r eval/requirements.txt          # numpy, scipy, soundfile; lame on the PATH for MP3
cmake -S . -B build -A x64 && cmake --build build --config Release
python eval/run.py                            # dev: 3 synthetic songs, all scenarios, all engines
python eval/run.py --set holdout43            # 20 synthetic songs nothing was tuned on
python eval/run.py --stems DIR --set test     # MUSDB18-HQ test songs 11-50
python eval/run.py --set holdout43 --metric leak  # another metric (scores are cached)
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
| `kar_lowpass` | karaoke band-limited: nothing above 16 kHz (a low-bitrate or video-site rip), without coding noise |
| `stereo_width` | karaoke's stereo image narrower (side at 60 %), as from another mix or master |

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
* **holdout43**: synthetic seeds 7000-7019, added for 4.3 and scored once, with the
  final code. Nothing was tuned on them.

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
| `v43` | plus the 2x2 EQ for a different stereo image, a band-limited karaoke's top band and the low-band start for wow: Utagoe Rip 4.3 |
| `v4-cpp` | the C++ port of `v43` (`src/engine/v4.cpp` through `tests/v4_cli`), what the program runs |
| `v41-cpp`, `v40-cpp` | the same port run as 4.1 (`v4_cli --v41`) and as 4.0 (`v4_cli --hard`) |

`proto.py` also keeps variants that were tried and not adopted (`proto-eq-ls1`,
`proto-eq-align-lvl`, `proto-eq-align-auto`, `v4-cv`, `v43-eqq`), for reproducibility. `v4`
(4.0) and `v41` share everything but the EQ estimate, the decision and the constant-lag
test; `v43` is `v41` with three options turned on (`mimo`, `noref`, `wow`).

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

A wobble the coarse windows missed can leave errors beyond the 2 kHz band's
unambiguous range (about 11 samples). 4.3 also runs the refinement starting below
500 Hz (unambiguous to about 44 samples), then 2 kHz and 6 kHz, and keeps that path
where the instrumental fits clearly better along it: each path gets its own EQ
estimate, and on the cells the instrumental dominates along the usual path (which
keeps the vocal out of the comparison) the low-band path must leave under 0.9 of the
residual (a threshold chosen on the dev songs). Always taking the low-band path helped
some songs by up to 4.5 dB and cost others up to 4.2 dB, where there was nothing to
reach and the low band's coarser phase only added noise. On the dev songs the test took
it for 2 of 13 (+0.2 and +4.5 dB) and lost nowhere; checked afterwards on the MUSDB test
songs' `wow` cases, it took it for 12 of 39, gaining up to 11.2 dB (*Mu - Too Bright*)
and losing 1.6 dB on one.

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

A per-channel gain cannot follow a karaoke whose stereo image differs (another width
or balance: each album channel then holds some of the other karaoke channel). For
stereo, 4.3 also fits a 2x2 EQ per bin (each output channel from both karaoke
channels, the same Huber weighting, a ridge of 10^-6 of the trace) and uses it where it
predicts held-out frames clearly better: both models are fitted on the even frames and
judged on the odd frames' cells the instrumental dominates, and the 2x2 one must leave
more than 20 % less residual. Where the image is the same its extra freedom only adds
estimation noise (always using it cost 1-2 dB on exact matches).

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

Tried for 4.3 and not adopted (`v43-eqq`): where the model can be exact but the EQ
difference has shape (more than 1.5 dB off its median between 100 Hz and 10 kHz),
reading the error at 0.25 instead of 0.10. On `album_eq` it left 3.4 dB less
instrumental in the pauses and scored 1 dB higher on average, but cost up to 4.8 dB of
vocal on some MUSDB songs (seven lost more than 1 dB).

**Band-limited karaoke (4.3).** A karaoke ripped from a video site or coded at a low
bitrate often has nothing above 16 kHz or so, while the album does. There is nothing to
subtract above the cut, so before 4.3 everything the album has there (cymbals, air)
passed into the vocal. The cut is found from the mean spectra: bins where the
karaoke's power is 20 dB under its usual share of the album's (1-8 kHz), those where
the album itself is silent counting either way; the top run of them (gaps under 500 Hz
bridged) must start below 20 kHz and hold at least 1 kHz where the album is not
silent; and the karaoke's own spectrum must fall by 20 dB across the cut (from the
750 Hz below it to the 750 Hz above, 250 Hz either side left out), which a dark
arrangement's gradual roll-off under a bright vocal does not. Above it, the album is kept in proportion to the vocal's share of the half
octave below the cut, frame by frame and smoothed over a few frames: the vocal's air
where it sings, nothing in its pauses. Zeroing the band instead scored nearly as well
but removes the vocal's own highs.

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

4.3, 4.1 and 4.0 here are the C++ separation as the program runs it (`v4-cpp`), and
as 4.1 and 4.0 ran it (`v41-cpp`, `v40-cpp`); the Python prototypes give the same
output to -55 dB or better. Median SDR in dB, higher is better. The differences are
per song, with a 95 % bootstrap confidence interval; "better / worse" counts the songs
where 4.3 differs from 4.1 by more than 0.05 dB, with the worst difference. The last
column is the same difference for leak (positive: 4.3 leaks less).

**Holdout synthetic songs** (`holdout43`: 20, added last and scored once, nothing tuned on them):

| Scenario | 3.0 | 4.0 | 4.1 | 4.3 | 4.3 − 3.0 [95 % CI] | 4.3 − 4.1 [95 % CI] | 4.3 vs 4.1: better / worse (worst) | leak, 4.3 better than 4.1 by |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `clean` | 27.50 | 27.56 | 57.56 | 57.56 | +30.05 [+29.14, +30.97] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `level` | 27.02 | 28.81 | 56.61 | 56.61 | +29.59 [+28.75, +30.46] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `inverted` | 27.48 | 27.56 | 50.83 | 50.83 | +23.35 [+22.58, +24.15] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `album_eq` | 3.18 | 25.98 | 31.67 | 31.67 | +28.49 [+27.92, +29.03] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `album_loud` | -3.86 | 9.43 | 10.81 | 10.81 | +14.67 [+14.05, +15.26] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `both_loud` | 22.08 | 22.88 | 23.80 | 23.80 | +1.71 [+1.04, +2.38] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `both_diff` | 7.66 | 22.53 | 22.75 | 22.75 | +15.09 [+14.37, +15.79] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `offset_frac` | 25.64 | 27.53 | 50.09 | 50.09 | +24.45 [+23.61, +25.37] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `drift` | 25.78 | 27.57 | 39.49 | 39.49 | +13.71 [+11.13, +16.41] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `drift_fast` | 19.41 | 26.63 | 31.68 | 31.68 | +12.27 [+11.06, +13.35] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `wow` | 17.52 | 24.28 | 22.86 | 23.07 | +5.55 [+4.98, +6.13] | +0.22 [+0.00, +0.52] | 3 / 0 (+0.00) | +0.98 [-0.14, +2.94] |
| `mp3` | 19.50 | 18.97 | 18.90 | 18.90 | -0.60 [-0.70, -0.50] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `everything` | 6.56 | 15.24 | 16.14 | 16.14 | +9.59 [+9.29, +9.86] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `kar_lowpass` | 14.51 | 14.48 | 14.72 | 31.24 | +16.73 [+15.65, +17.74] | +16.52 [+15.41, +17.55] | 20 / 0 (+10.83) | +19.80 [+19.46, +20.12] |
| `stereo_width` | 17.20 | 17.80 | 15.58 | 53.92 | +36.73 [+36.13, +37.29] | +38.34 [+37.79, +38.87] | 20 / 0 (+35.43) | +39.19 [+38.46, +39.91] |

The `holdout41`, `final41`, `final` and `fresh` songs give the same picture for 4.1.

**MUSDB18-HQ test songs 11-50** (30 s excerpts; 39 songs, see below):

| Scenario | 3.0 | 4.0 | 4.1 | 4.3 | 4.3 − 3.0 [95 % CI] | 4.3 − 4.1 [95 % CI] | 4.3 vs 4.1: better / worse (worst) | leak, 4.3 better than 4.1 by |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `clean` | 23.67 | 23.78 | 49.71 | 49.71 | +26.04 [+22.90, +29.02] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `level` | 23.51 | 25.30 | 49.64 | 49.64 | +26.13 [+23.16, +28.95] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `inverted` | 23.67 | 23.78 | 47.01 | 47.01 | +23.34 [+20.87, +25.70] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `album_eq` | 5.45 | 22.62 | 31.06 | 31.06 | +25.61 [+24.19, +26.80] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `album_loud` | -4.37 | 11.65 | 12.73 | 12.78 | +17.16 [+16.14, +18.12] | +0.05 [+0.00, +0.15] | 1 / 0 (+0.00) | +0.04 [+0.00, +0.11] |
| `both_loud` | 17.39 | 19.25 | 20.23 | 20.23 | +2.84 [+1.79, +4.18] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `both_diff` | 7.09 | 17.76 | 18.31 | 18.31 | +11.22 [+10.39, +12.02] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `offset_frac` | 22.26 | 23.75 | 48.74 | 48.74 | +26.48 [+23.56, +29.30] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `drift` | 22.47 | 23.76 | 45.00 | 45.00 | +22.53 [+20.16, +24.84] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `drift_fast` | 19.13 | 23.22 | 42.78 | 42.78 | +23.65 [+20.73, +26.35] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `wow` | 18.08 | 19.96 | 21.67 | 22.44 | +4.36 [+3.29, +5.49] | +0.77 [+0.25, +1.45] | 10 / 2 (-1.61) | +1.48 [+0.23, +3.01] |
| `mp3` | 19.54 | 19.45 | 19.87 | 19.87 | +0.33 [+0.04, +0.60] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | +0.00 [+0.00, +0.00] |
| `everything` | 6.19 | 14.81 | 15.07 | 15.07 | +8.87 [+8.13, +9.65] | +0.00 [+0.00, +0.00] | 0 / 0 (+0.00) | -0.00 [-0.00, +0.00] |
| `kar_lowpass` | 21.12 | 21.22 | 26.74 | 32.44 | +11.32 [+9.42, +13.33] | +5.70 [+3.96, +7.55] | 33 / 0 (+0.00) | +11.14 [+8.93, +13.32] |
| `stereo_width` | 16.90 | 17.60 | 16.38 | 43.35 | +26.45 [+21.78, +30.86] | +26.97 [+22.12, +31.58] | 32 / 0 (+0.00) | +30.50 [+25.36, +35.28] |

Songs 11-50 are 40 songs; *Skelpolu - Resurrection* has no vocal in its excerpt, so its
median SDR is undefined (it still counts for leak, where 36 songs have pauses).

**Whole songs.** The tables use 30 s excerpts. On six whole songs (MUSDB18-HQ test
songs 1-3 and 11-13, `--seconds 600 --start 0`), 4.1's lead over 3.0 per scenario was:
`clean` +32.7 / +35.1 dB (songs 1-3 / 11-13), `drift` +27.9 / +30.3, `drift_fast` +30.6
/ +33.6, `both_loud` +3.8 / +2.3, `both_diff` +12.4 / +10.7, `wow` +8.4 / +3.2,
`everything` +7.6 / +9.3; 4.1 was better on every song in each of these. Against 4.0 it
was 1.2-1.7 dB ahead on songs 1-3 and 0.2-0.5 dB behind on songs 11-13 where the
releases' dynamics differ (`both_loud`, `both_diff`). 4.3 gave 4.1's results on the
same songs and scenarios except `wow`, where its low-band start was taken on two songs:
+3.7 dB on one, -0.5 dB on the other.

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
  confirmed them. 4.3's additions were chosen on the dev songs; the wow test was then
  checked on the test songs' `wow` rows (and kept), and a fourth change (`v43-eqq`,
  under Soft decision) was dropped after the test songs' `album_eq` rows showed it
  cost some of them vocal. The fixes are general, but the MUSDB
  numbers are optimistic by an unknown amount. The final synthetic songs are clean but
  synthetic. A clean real-music check
  would need songs nothing was tuned on, such as the MUSDB18-HQ training set (the same
  22.7 GB download).
* **Where 4.3 is not better.** Wow can still defeat the tracking: with 2 s windows
  against a 1.8 s wobble, the coarse curve can lose a stretch. 4.3's low-band start
  recovers the worst MUSDB case (*Mu - Too Bright*: 4.1 was 9 dB below 3.0 on `wow`,
  4.3 is 2 dB above it), but five songs are still up to 2 dB below 3.0. On the
  synthetic songs 4.3 is about 1.2 dB below 4.0 on `wow`: where the lag wobbles, the
  soft decision keeps more of what the delete rule removed. With MP3 files, 4.3 (like
  4.1) leaves a little more coding noise in the vocal's pauses than 3.0 (leak 1-2.5 dB
  higher) and scores 0.5 dB below it on the synthetic songs; the `mp3` rows are
  approximate (above). Keeping more of every cell also means that where the model is
  imperfect, 4.1 and 4.3 leave more instrumental in the pauses than 4.0 (`album_eq`:
  about 10 dB more leak than 4.0, still 29 dB less than 3.0). Two ways of bringing that
  down were tried for 4.3: fading frames whose residual is no more than the model's
  typical error (the vocal's pauses), and reading the error higher where the EQ has
  shape (`v43-eqq`). Both took vocal away on some songs, so neither was adopted.
* **Speed.** The separation reads both files several times (lag search, tracking, the
  EQ's passes, level tracking, output), using all processor cores: a 3:20 song takes
  about 22 s on a 16-thread desktop (26 s when the timing wobbles and both refinement
  paths run), against 20 s for 4.1 and 6 s for 3.0.
