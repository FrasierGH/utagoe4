// Reading audio files of any common format (4.2): WAV of any sample format ourselves, and
// everything else (MP3, AAC/M4A, FLAC, ALAC, WMA, ...) through Windows' Media Foundation
// decoders. The processing works at 16 bits, so a file is decoded, converted to the rate
// and channel count it is processed at, and rounded to 16 bits (no dither: two identical
// high-resolution files stay identical, so they still cancel exactly).
#pragma once

#include <string>

#include "wav.hpp"

namespace utagoe {

struct AudioInfo {
    std::wstring codec;  // "PCM", "Float", "MP3", "AAC", "FLAC", ...
    int rate = 0, channels = 0;
    int bits = 0;        // bits per sample of a lossless format, 0 for a lossy one
};

// 0 or WAV_OPEN_FAILED (cannot open) / WAV_BAD_FILE (not audio, damaged, or no decoder).
int audio_info(const std::wstring& path, AudioInfo* info);

// Decodes `path` into 16-bit audio at `rate` and `channels` (0: the file's own; more than
// two channels are mixed down to stereo). A different rate is converted with a windowed
// sinc; mono becomes stereo by copying, stereo becomes mono by averaging.
bool load_audio(const std::wstring& path, int rate, int channels, Audio* out);

// The audio file extensions the program offers in its dialogs and searches for
// (upper case, with the dot).
extern const wchar_t* const AUDIO_EXTENSIONS[];
bool is_audio_extension(const std::wstring& upper_ext);

}  // namespace utagoe
