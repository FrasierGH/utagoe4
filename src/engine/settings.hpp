// Processing settings: the [V30_Option] section of the original UtagoeRip.ini
// (raw positions of the Settings dialog controls) and the conversions the
// original performs before processing (TSetForm1, 0x409c4c).
#pragma once

#include <string>

namespace utagoe {

enum { PROC_NORMAL, PROC_LR_DIFF, PROC_MONO };
enum { MERGE_FREQUENCY, MERGE_WAVEFORM };
enum { INTRO_AUTO, INTRO_NORMAL, INTRO_DETAILED, INTRO_NONE };
enum { LEVEL_AUTO_AVERAGED, LEVEL_AUTO_ADAPTIVE, LEVEL_MANUAL, LEVEL_NONE };
enum { ADPT_AUTO, ADPT_MANUAL };
enum { PHASE_AUTO, PHASE_POSITIVE, PHASE_INVERTED };
enum { QUALITY_PRIORITY, EXTRACTION_PRIORITY };

struct Settings {
    int proc_mode = PROC_NORMAL;            // ProcMode
    int merge_mode = MERGE_FREQUENCY;       // MergeMode
    bool ovsp_flag = false;                 // OvspFlg
    int ovsp_mx = 32;                       // OvspMx
    int blk_size = 100;                     // BlkSize (ms)
    int adpt_mode = ADPT_AUTO;              // AdptMode
    int adpt_num = 3;                       // AdptNum
    bool cntr_flag = false;                 // CntrFlg
    int cntr_pos = 6;                       // CntrPos (0..20)
    int intro_mode = INTRO_AUTO;            // IntroMode
    int level_adpt = LEVEL_AUTO_AVERAGED;   // LevelAdpt
    bool lpf_flag = false;                  // LPFFlg
    int lpf_pos = 10;                       // LPFPos
    bool hpf_flag = false;                  // HPFFlg
    int hpf_pos = 10;                       // HPFPos
    int kvol_pos = 6;                       // KvolPos
    int klvl_pos = 10;                      // KlvlPos
    int krk_phase = PHASE_AUTO;             // KrkPhase
    int sound_qty = QUALITY_PRIORITY;       // SoundQty
    bool kname_flg = true;                  // KnameFlg
    bool vname_flg = true;                  // VnameFlg
    std::wstring vname_txt = L"_vo";        // VnameTxt
    std::wstring language;                  // Language (not in the original: "ja", "en" or "")
    bool v4 = true;                         // V4Engine: Utagoe Rip 4 separation (not in the original)

    // derived values
    int block_ms() const { return blk_size < 50 ? 50 : blk_size; }
    double block_sec() const { return block_ms() * 0.001; }
    int oversample() const { return ovsp_mx < 1 ? 1 : ovsp_mx; }
    int adpt_range() const { return (int)(block_ms() * 0.01 * (adpt_num < 0 ? 0 : adpt_num)); }
    float cntr_strength() const { return (float)(cntr_pos * 0.25 + 0.5); }
    int lpf_hz() const { return lpf_pos * 800 + 2000; }
    int hpf_hz() const { return (int)((hpf_pos + 1) * 1.5 * (hpf_pos + 1) + 49.0); }
    float kvol() const {
        return kvol_pos < 10 ? (float)(kvol_pos * 0.1 + 0.6) : (float)((kvol_pos - 9) * 0.3 + 1.5);
    }
    float klvl() const { return (float)(klvl_pos * 0.03 + 0.7); }

    // UtagoeRip.ini ([V30_Option], Shift-JIS, CRLF)
    bool load_ini(const std::wstring& path);
    bool save_ini(const std::wstring& path) const;
};

}  // namespace utagoe
