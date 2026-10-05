// Processing driver: automatic analysis, block loop and post-processing
// (TVocalMain / TVocalFunc in the original).  See docs/ALGORITHM.md.
#pragma once

#include <functional>
#include <string>

#include "settings.hpp"
#include "wav.hpp"

namespace utagoe {

struct Analysis {
    int ofs0 = 0;      // initial offset of the instrumental (samples)
    int phase = 0;     // 1 = instrumental is phase inverted
    int base = 0;      // per-block drift correction
    int range = 0;     // per-block search radius
    double vol = 1.0;  // instrumental level
};

// The original keeps the last analysis and reuses it for the same pair of
// files, processing mode and block length.
struct AnalysisCache {
    bool valid = false;
    std::wstring orig, inst;
    int proc_mode = 0;
    double block_sec = 0.0;
    Analysis analysis;
};

enum Status { STATUS_NONE, STATUS_ANALYZING, STATUS_PREPARING };

struct Callbacks {
    std::function<void(const std::wstring&)> log;  // debug-log lines (English)
    std::function<void(double)> progress;          // 0..1, every 12 blocks like the original
    std::function<void(Status)> status;
    std::function<bool()> cancel;
    std::function<void()> timestamp;               // "N seconds elapsed" point in the log
};

struct TrialResult {
    double ofs = 0, voc = 0, org = 0, bnsn = 0, vol = 1;
};

class UtagoeRip {
public:
    // inst == nullptr: process the original on its own (filters / centre only)
    UtagoeRip(const Audio& orig, const Audio* inst, const Settings& cfg, Callbacks cb = {},
              bool original_timing = false, AnalysisCache* cache = nullptr,
              const std::wstring& orig_key = L"", const std::wstring& inst_key = L"");

    Audio run();
    bool cancelled() const { return cancelled_; }
    const Analysis& analysis() const { return analysis_; }

    // exposed for tests / tools
    TrialResult trial(int ofs0, int base, bool level_search, int phase, int max_sec, int rng);
    int intro_simple(int phase);
    int intro_detailed(bool mono, int phase);

private:
    struct Cancelled {};
    void check();
    void log(const std::wstring& s) { if (cb_.log) cb_.log(s); }
    void status(Status s) { if (cb_.status) cb_.status(s); }
    void progress(double f) { if (cb_.progress) cb_.progress(f); }
    void log_trial(int rng, const TrialResult& t);
    void initial_offset(int* ofs, int* phase);
    bool auto_analysis();
    void do_auto_analysis();
    Audio run_original_only();
    Audio run_v4();  // Utagoe Rip 4 separation (v4.cpp), then the same post-processing
    Audio finish(std::vector<double> ch[2]);

    const Audio& orig_;
    const Audio* inst_;
    Settings cfg_;
    Callbacks cb_;
    bool original_timing_;
    AnalysisCache* cache_;
    std::wstring orig_key_, inst_key_;
    int ch_, rate_;
    bool cancelled_ = false;
    Analysis analysis_;
};

}  // namespace utagoe
