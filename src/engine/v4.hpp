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
};

// mix and kar: planar, the same number of channels (1 or 2), samples in [-1, 1].
// Returns the vocal, planar, as long as mix. progress(0..100) may return false to cancel
// (then the result is empty).
std::vector<std::vector<double>> separate(const std::vector<std::vector<double>>& mix,
                                          const std::vector<std::vector<double>>& kar, int rate,
                                          const Options& opt, Report* report = nullptr,
                                          const std::function<bool(int)>& progress = nullptr);

}  // namespace v4
}  // namespace utagoe
