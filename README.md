# 歌声りっぷ (Utagoe Rip) 4 — in development

This is the development line after the faithful 3.0 rebuild (which stays as it is at
[FrasierGH/utagoe](https://github.com/FrasierGH/utagoe)). The goal is better
extraction, measured against 3.0 rather than judged by ear:

1. **Per-band EQ matching**: subtract the instrumental as it was EQ'd in the album
   master, not just at the right level. Prototyped: on mastered albums it lifts the
   result from 1–3 dB to about 26 dB SDR (see [eval/README.md](eval/README.md)).
2. **Time-varying, sub-sample alignment**: handle clock drift between releases.
3. **Soft masking**: fewer "watery" artifacts than keeping or deleting whole bins.
4. **Multi-resolution analysis**: sharper consonants and drums.
5. **More formats**: MP3/FLAC/AAC/24-bit input, float output, mismatched rates.
6. **High-DPI** support and workflow features (batch, A/B preview).

Ideas are prototyped in Python under `eval/`, scored there, and the winners are
ported to the C++ engine. Until then, everything below describes the 3.0 program
this line starts from.

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

Get `utagoe.exe` from the [Releases](../../releases) page and run it. Settings
are kept in `%LOCALAPPDATA%\UtagoeRip\utagoe.ini`.

Every release is built by GitHub Actions from the tagged source
(`.github/workflows/build.yml`). The release lists the exe's SHA-256, and the
build is signed with a GitHub [build provenance
attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations).
To check that a download came from this repository's source, use the
[GitHub CLI](https://cli.github.com/):

```bash
gh attestation verify utagoe.exe --repo FrasierGH/utagoe
```

You can also build it yourself (see below).

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
mode recovers the vocal. It also covers the offset search, inverted phase,
the settings and the INI format. `engine_test ORIGINAL INSTRUMENTAL OUTPUT
[Key=Value ...]` processes files from the command line and prints the
analysis log.

### Releasing

`.github/workflows/build.yml` builds and tests on every push. Pushing a tag
such as `v3.0.0` also creates a GitHub Release with `utagoe.exe` and its
checksum attached:

```bash
git tag v3.0.0
git push origin v3.0.0
```

## Repository layout

| Path | Contents |
|---|---|
| `src/engine/` | the processing: WAV I/O, settings and the INI format, FFT, `ThVocalFFT`, `CenterFocus`, FIR filters (`dsp`), analysis and block processing (`engine`) |
| `src/app/` | the four windows, a small Win32 layer that reproduces the VCL controls (`ui`), the language tables, the file-name logic, and the resource script |
| `res/` | icon, artwork and help ripped from the original, plus the English help |
| `tests/engine_test.cpp` | the test suite and command-line harness |
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
