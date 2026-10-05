# 歌声りっぷ (Utagoe Rip) 4

The faithful 3.0 rebuild lives on, unchanged, at
[FrasierGH/utagoe](https://github.com/FrasierGH/utagoe). This line adds a better
separation: **Utagoe Rip 4**. It is on by default (Settings > Misc > *Improved
extraction*); turning it off gives exactly the original's processing.

3.0 subtracts the karaoke at one level, and searches small block-by-block offsets.
v4 additionally:

* **matches EQ differences** between the album and the karaoke release, per
  frequency band, and **level differences over time** when their dynamics differ
  (the album compressed and limited differently from the karaoke);
* **aligns frame by frame, to a fraction of a sample**, following clock drift and
  tape or vinyl speed wobble;
* **finds the offset and polarity itself**, telling the true lag from a repeat a few
  bars away in loop-based music.

It keeps 3.0's per-bin decision rule, so where both releases really are the same
instrumental the result is about the same.

| Album vs karaoke release (30 s excerpts, median SDR in dB) | 3.0 | v4 |
|---|---:|---:|
| Identical instrumental | 23.7 | 23.8 |
| Karaoke at a different level | 23.5 | 25.3 |
| Album EQ'd for mastering | 5.5 | 22.6 |
| Album loudly mastered (EQ, compressor, limiter) | -4.4 | 11.7 |
| Both mastered, each with its own dynamics | 17.4 | 19.3 |
| Karaoke from a different master | 7.1 | 17.8 |
| Fractional-sample offset | 22.3 | 23.8 |
| Clock drift, 30 ppm / 300 ppm | 22.5 / 19.1 | 23.8 / 23.2 |
| Vinyl or tape wow | 18.1 | 20.0 |
| Remaster + level + offset + drift + MP3 | 6.2 | 14.8 |

MUSDB18-HQ test songs 11-50 (39 with a vocal in the excerpt), each release pair built
from the song's stems; higher is better. On synthetic
songs nothing was tuned on, the picture is the same. v4 is not better everywhere: see
the limitations in [eval/README.md](eval/README.md#results) (one song loses 11 dB to
wow; MP3 coding noise costs a little; v4 takes about three times as long).

Everything about how this was measured, and how v4 works, is in
[eval/README.md](eval/README.md). The separation is `src/engine/v4.cpp`; the Python
prototype it was developed from is `eval/proto.py`, and the two give the same output
(to -60 dB or better).

**Still planned:** soft masking (fewer "watery" artifacts than keeping or deleting
whole bins), multi-resolution analysis (sharper consonants and drums), more input
formats (MP3/FLAC/AAC/24-bit, float output, mismatched sample rates), high-DPI
support and workflow features (batch, A/B preview).

---

# 歌声りっぷ (Utagoe Rip)

**Utagoe Rip** is a Japanese freeware tool by TODAKEN (1999–2009) that
extracts the **vocals** from a song when you also have its **instrumental /
karaoke / off-vocal** version. It doesn't just phase-invert one waveform
against the other. It subtracts the instrumental *per frequency bin* and drops
every bin the instrumental explains, so small timing, level and phase
differences between the two files don't leave the usual residue.

The original Windows program has long been out of distribution. This
repository brings it back as an **open-source C++ rebuild of version 3.0**,
reconstructed from a disassembly of the original:

* **The same program.** The main window, Settings (all three tabs), Playback
  and About are replicas of the original's. They have the same layout,
  controls, artwork and behaviour, down to the hidden debug mode and the logo
  animation. In English mode every window matches DjLizard's en_US build pixel
  for pixel when the two run side by side on Windows 10. The layout matches
  the Japanese original as well.
* **The same processing**: automatic analysis, alignment, spectral
  subtraction, filters, and every option in the Settings dialog.
* **Two languages.** On a Japanese Windows it looks exactly like TODAKEN's
  original. Anywhere else it is in English, following DjLizard's 2013 en_US
  build with the messages that build left in Japanese translated.
* **One small file.** `utagoe.exe` is about 0.8 MB, like the original. It
  needs no installation and no runtime DLLs, only Windows' own.

## Download

Utagoe Rip 4 has no release yet: build it yourself (see below). The 3.0 rebuild
is released at [FrasierGH/utagoe](https://github.com/FrasierGH/utagoe/releases).
Settings are kept in `%LOCALAPPDATA%\UtagoeRip\utagoe.ini`, shared with 3.0
(Utagoe Rip 4 adds one key, `V4Engine`).

Releases are built by GitHub Actions from the tagged source
(`.github/workflows/build.yml`). A release lists the exe's SHA-256, and the
build is signed with a GitHub [build provenance
attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations).
To check that a download came from this repository's source, use the
[GitHub CLI](https://cli.github.com/):

```bash
gh attestation verify utagoe.exe --repo FrasierGH/utagoe4
```

## Using it

1. Drop the original song (a 16-bit WAV) onto the top box, or pick it with the
   folder button. The instrumental next to it is found automatically, and the
   output name (`<song>_vo.wav`) is filled in.
2. Press **Start**. Progress is shown at the bottom right and in the title bar.
3. Listen to the result with the speaker button.

**Help...** opens the manual: an English translation of TODAKEN's
`UtagoeHelp.pdf`, or the original PDF in Japanese mode.

* **Language**: follows Windows. To force one, add `Language=ja` or
  `Language=en` to `[V30_Option]` in the INI file, or start the program with
  `--lang ja` / `--lang en`.
* **Same file in both input boxes**: applies only Extraction Centralization
  and the filters to the original.
* **Hidden debug mode** (as in the original): double-click the invisible
  8×8 px spot at the bottom-left of the main window. The analysis log is then
  written to `<original>.txt`.

### Differences from the original

* **Output timing.** The original's FFT stages delay the vocal by 7168
  samples (about 160 ms at 44.1 kHz), plus another 7168 with centralization.
  It never corrects this, so its output is out of sync with the song. This
  version outputs the vocal in sync.
* The output always has exactly the length of the original song.
* **Unicode.** The original was an ANSI program. File names outside the
  system code page didn't work, playback failed for long paths, and on
  non-Japanese Windows its Japanese text showed as `????`. This version has
  none of these problems.
* Processing runs in a background thread, so the window stays responsive.
* Double instead of single precision in a few places, so results can differ
  from the original's in the least significant bit.
* **Utagoe Rip 4 separation** (see the top of this page), on by default. Turned
  off, the processing is the original's.

## Building

The source is plain C++17 and the Win32 API, built with CMake (3.16 or later).
Either compiler works.

**Visual Studio** 2022 or later (or the free Build Tools, with the *Desktop
development with C++* workload):

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The program is `build/Release/utagoe.exe`.

**MinGW-w64** (GCC):

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build
```

The program is `build/utagoe.exe`.

`ctest` runs `engine_test`, which builds a synthetic song (a known "vocal"
over an instrumental, with an offset instrumental file) and checks that every
mode recovers the vocal, with 3.0's processing and with Utagoe Rip 4's (including
an inverted and a drifting instrumental). It also covers the offset search,
the settings and the INI format. `engine_test ORIGINAL INSTRUMENTAL OUTPUT
[Key=Value ...]` processes files from the command line with 3.0's processing
(`V4Engine=1` for v4) and prints the analysis log. `v4_cli ORIGINAL INSTRUMENTAL
OUTPUT` runs the v4 separation alone and prints what it found (offset,
polarity, drift, level tracking). The benchmark against 3.0 is in `eval/` (Python).

### Releasing

`.github/workflows/build.yml` builds and tests on every push. Pushing a tag
such as `v4.0.0` also creates a GitHub Release with `utagoe.exe` and its
checksum attached:

```bash
git tag v4.0.0
git push origin v4.0.0
```

## Repository layout

| Path | Contents |
|---|---|
| `src/engine/` | the processing: WAV I/O, settings and the INI format, FFT, `ThVocalFFT`, `CenterFocus`, FIR filters (`dsp`), analysis and block processing (`engine`), the Utagoe Rip 4 separation (`v4`) |
| `src/app/` | the four windows, a small Win32 layer that reproduces the VCL controls (`ui`), the language tables, the file-name logic, and the resource script |
| `res/` | icon, artwork and help ripped from the original, plus the English help |
| `tests/engine_test.cpp` | the test suite and command-line harness |
| `tests/v4_cli.cpp` | the v4 separation from the command line |
| `eval/` | the benchmark against 3.0 and the Python prototype of v4 ([eval/README.md](eval/README.md)) |
| `tools/rip_assets.py` | re-extracts the artwork from an original `utagoe.exe` (Python) |
| `docs/ALGORITHM.md` | how the original works, with addresses in the binary |
| `docs/readme_ja.txt` | the original readme |
| `original/` | (ignored) put copies of the original program here |

## Credits and license

Utagoe Rip, its name, artwork and help are by TODAKEN. The original readme
says: 「転載・配付は自由です。商利用についてはご連絡ください。」("You may freely
repost and redistribute it. Please get in touch about commercial use."). They
are redistributed here on those terms. The 2013 partial English translation is
by DjLizard.

The code in this repository is released under the MIT license (see
`LICENSE`).
