// Utagoe Rip 4 separation: per-band EQ matching, frame-by-frame alignment (drift,
// wow, polarity) and automatic level tracking around 3.0's per-bin decision rule.
// A C++ port of eval/proto.py's `v4` engine; the evaluation harness scores the two
// against each other (engine "v4-cpp").
#pragma once

#include <functional>
#include <vector>

namespace utagoe {
namespace v4 {

struct Options {
    double kvol = 1.2;        // Extractable Level (3.0's kvol); cap = min(kvol, 1.5)
    bool quality = true;      // Accuracy Priority: Quality (phase test) or Extraction
    double octave = 1.0 / 3;  // EQ smoothing
    int level_track = 2;      // 0 off, 1 always, 2 auto (when it holds up out of sample)
    double lvl_gain = 0.13;   // auto: estimated on even bins, must cut the odd bins' residual this much
    bool huber = true;        // EQ by Huber-weighted least squares (4.1); false: 4.0's two passes
    int huber_iters = 2;
    bool soft = true;         // soft decision per cell (4.1); false: 3.0's keep-or-delete rule
    double const_lag = 0.1;   // a fitted drift under this many samples counts as none (4.1; 4.0: 0)
    bool noref = true;        // a band-limited karaoke: the mix above its cut by the vocal's share (4.3)
    bool wow = true;          // a wobbling lag: also try the refinement from below 500 Hz (4.3)
    bool mimo = true;         // stereo: a 2x2 EQ where it predicts clearly better (4.3; needs huber)
};

struct Report {
    long lag = 0;              // samples: karaoke[t + lag] ~ instrumental in the mix[t]
    int sign = 1;              // -1: karaoke polarity inverted
    bool drift_line = true;    // the lag over time fitted a straight line (offset + drift)
    double lag_start = 0, lag_end = 0;
    double level_gain = 0;     // out-of-sample residual reduction of the level correction
    bool level_applied = false;
    double stretch = 0;        // typical lag change across one frame, samples
    bool resampled = false;    // the karaoke was resampled along the lag curve (stretch > 0.6)
    double soft_q = 0;         // soft decision: the quantile the model error was read at (0: hard rule)
    bool wow_lowband = false;  // the lag's refinement started below 500 Hz (a wobble out of range)
    double lowpass_hz = 0;     // the karaoke's cut, where it is band-limited (0: it is not)
    bool mimo = false;         // the 2x2 EQ was used (the karaoke's stereo image differs)
};

// mix and kar: planar, the same number of channels (1 or 2), samples in [-1, 1].
// Returns the vocal, planar, as long as mix. progress(0..100) may return false to cancel
// (then the result is empty).
std::vector<std::vector<double>> separate(const std::vector<std::vector<double>>& mix,
                                          const std::vector<std::vector<double>>& kar, int rate,
                                          const Options& opt, Report* report = nullptr,
                                          const std::function<bool(int)>& progress = nullptr,
                                          std::vector<std::vector<double>>* instrumental = nullptr);
// instrumental: if given, also the instrumental as subtracted (the karaoke matched to
// the mix in timing, polarity, EQ and level), planar, as long as mix.

}  // namespace v4
}  // namespace utagoe
