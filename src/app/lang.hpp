// User-interface text for the two builds of the original: JA reproduces
// TODAKEN's Japanese release (captions, font and the few layout differences of
// its forms); EN follows DjLizard's en_US build, with the messages it left in
// Japanese translated.
#pragma once

#include <string>

namespace utagoe {

struct Pt { int x, y; };

struct Lang {
    bool japanese;
    const wchar_t* font;
    unsigned char charset;
    const wchar_t *warning, *confirm, *ok, *cancel, *wave_files, *all_files;
    // main window
    const wchar_t *label1, *label2, *label3, *start, *abort, *quit, *settings_btn, *help_btn, *about_hint;
    // messages
    const wchar_t *drop_wave, *cannot_open, *bad_file, *bad_format, *info, *mono, *stereo, *confirm_abort,
        *need_names, *same_file, *rate_mismatch, *need_16bit, *overwrite, *create_failed, *help_missing,
        *help_reader, *help_error, *debug, *no_memory, *write_error;
    const wchar_t* help_resource;  // HELP_EN / HELP_JA
    const wchar_t* help_file;
    const wchar_t *analyzing, *preparing, *elapsed;
    // settings
    const wchar_t *settings_title, *tab1, *tab2, *tab3, *reset;
    const wchar_t* intro[5];   // caption + items
    const wchar_t* merge[3];
    const wchar_t* level[5];
    const wchar_t* qty[3];
    const wchar_t* data[4];
    const wchar_t* phase[4];
    const wchar_t *adpt_group, *adpt_auto, *adpt_manual, *adpt_range, *kvol_label, *weak, *strong,
        *filter_group, *cfocus, *lpf, *hpf, *ovsp_group, *ovsp_check, *ovsp_note, *ovsp_unit, *bsize_group,
        *bsize_unit, *file_group, *kname, *vname, *vname_label;
    Pt pos_kvol_label, pos_strong;
    int w_cfocus, w_filter_check, pos_bsize_group;
    Pt pos_bsize_combo, pos_bsize_unit;
    int ovsp_note_x, ovsp_note_y, ovsp_note_w, ovsp_note_h;  // w = 0: auto-size, left aligned
    int pos_vname_label, pos_vname_edit;
    // playback / about
    const wchar_t *play_title, *about_title, *credits;
    int about_height, about_bevel, credits_x, credits_y, credits_w, credits_h, about_ok_top;
    // Utagoe Rip 4 (Misc tab)
    const wchar_t *v4_group, *v4_check, *v4_note;
    // 4.2: input formats other than 16-bit WAV
    const wchar_t *audio_files;  // the input dialogs' filter name
    const wchar_t *info_codec;   // the format label of a compressed file: rate, codec, channels
    const wchar_t *channels_n;   // more than two channels
};

extern const Lang LANG_EN;
extern const Lang LANG_JA;

// "ja"/"en", or empty for the Windows display language
const Lang& pick_language(const std::wstring& code);

// Engine log lines (English) as the Japanese build's debug log writes them
std::wstring log_line(const Lang& lang, const std::wstring& line);

}  // namespace utagoe
