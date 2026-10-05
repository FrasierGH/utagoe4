// TSetForm1: the Settings dialog.
#include <windows.h>
#include <commctrl.h>

#include "app.hpp"
#include "logic.hpp"

namespace utagoe {

namespace {

enum {
    IDC_TAB = 200, IDC_RESET,
    IDC_INTRO = 210, IDC_MERGE = 220, IDC_LEVEL = 230, IDC_QTY = 240, IDC_DATA = 250, IDC_PHASE = 260,
    IDC_ADPT_AUTO = 270, IDC_ADPT_MANUAL, IDC_ADPT_EDIT, IDC_ADPT_UPDOWN,
    IDC_KVOL = 280, IDC_KLVL, IDC_CFOCUS_CHK, IDC_CFOCUS, IDC_LPF_CHK, IDC_LPF, IDC_HPF_CHK, IDC_HPF,
    IDC_OVSP_CHK = 290, IDC_OVSP, IDC_BSIZE, IDC_KNAME, IDC_VNAME, IDC_VNAME_EDIT,
    IDC_V4 = 300,
};

int checked(const std::vector<HWND>& radios) {
    for (size_t i = 0; i < radios.size(); i++)
        if (SendMessageW(radios[i], BM_GETCHECK, 0, 0) == BST_CHECKED) return (int)i;
    return 0;
}

void check_one(const std::vector<HWND>& radios, int index) {
    for (size_t i = 0; i < radios.size(); i++)
        SendMessageW(radios[i], BM_SETCHECK, (int)i == index ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool is_checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void set_check(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
int track_pos(HWND h) { return (int)SendMessageW(h, TBM_GETPOS, 0, 0); }

int str_to_int_def(const std::wstring& s, int def) {
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    while (*end == L' ') end++;
    return *end ? def : (int)v;
}

void clip_width(HWND h, int right) {
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, GetParent(h), (POINT*)&r, 2);
    SetWindowPos(h, nullptr, 0, 0, right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

}  // namespace

void SettingsForm::run() {
    font = main_->font;
    if (!create(main_->L().settings_title, 506, 337, WS_POPUP | WS_CAPTION, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                 main_->hwnd))
        return;
    build();
    load(main_->cfg);
    show_page(0);
    SetFocus(GetDlgItem(hwnd, IDOK));
    show_modal(main_->hwnd);
}

void SettingsForm::build() {
    const Lang& l = main_->L();
    // TabOrder: OkBitBtn, CanBitBtn, PageControl, ResetButton
    bitbtn(hwnd, IDOK, 308, 305, 89, 25, l.ok, main_->glyph_ok, -1, 4, BS_DEFPUSHBUTTON);
    bitbtn(hwnd, IDCANCEL, 409, 305, 89, 25, l.cancel, main_->glyph_cancel);
    tab_ = child(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPCHILDREN, WS_EX_CONTROLPARENT, hwnd, IDC_TAB, 8, 8, 489, 291);
    const wchar_t* tabs[3] = {l.tab1, l.tab2, l.tab3};
    for (int i = 0; i < 3; i++) {
        TCITEMW it = {TCIF_TEXT};
        it.pszText = (LPWSTR)tabs[i];
        SendMessageW(tab_, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    button(hwnd, IDC_RESET, 200, 305, 75, 25, l.reset);
    RECT disp;
    GetClientRect(tab_, &disp);
    SendMessageW(tab_, TCM_ADJUSTRECT, FALSE, (LPARAM)&disp);
    disp.top += 2;  // TCustomTabControl.GetDisplayRect
    for (int i = 0; i < 3; i++)
        pages_[i] = page(tab_, disp.left, disp.top, disp.right - disp.left, disp.bottom - disp.top);

    // --- Processing Method
    HWND p = pages_[0];
    intro_ = radio_group(p, IDC_INTRO, 8, 8, 120, 128, l.intro, 4);
    group(p, 8, 142, 120, 107, l.adpt_group);
    enter(8, 142, 120, 107);
    adpt_auto_ = radio(p, IDC_ADPT_AUTO, 15, 160, 91, 17, l.adpt_auto, true);
    adpt_manual_ = radio(p, IDC_ADPT_MANUAL, 15, 188, 91, 17, l.adpt_manual, false);
    adpt_edit_ = edit(p, IDC_ADPT_EDIT, 71, 217, 35, 20, 3);
    adpt_updown_ = updown(p, IDC_ADPT_UPDOWN, adpt_edit_, 106, 16, 999);
    static_text(p, 18, 221, l.adpt_range);
    leave();
    merge_ = radio_group(p, IDC_MERGE, 135, 8, 338, 241, l.merge, 2, 232);  // the groups on top hide the rest
    qty_ = radio_group(p, IDC_QTY, 232, 24, 233, 88, l.qty, 2, 309);
    static_text(p, l.pos_kvol_label.x, l.pos_kvol_label.y, l.kvol_label);
    kvol_ = trackbar(p, IDC_KVOL, 309, 56, 154, 40, 20, 15, true);
    static_text(p, 312, 91, l.weak);
    kvol_text_ = static_text(p, 377, 94, L"1.0");
    static_text(p, l.pos_strong.x, l.pos_strong.y, l.strong);
    child(L"STATIC", L"", SS_ETCHEDHORZ, 0, p, -1, 144, 118, 322, 2);
    level_ = radio_group(p, IDC_LEVEL, 232, 126, 233, 115, l.level, 4);
    clip_width(level_[2], sx(309));
    clip_width(level_[3], sx(309));
    klvl_ = trackbar(p, IDC_KLVL, 309, 184, 154, 40, 20, 15, true);
    klvl_text_ = static_text(p, 374, 220, L"1.00");

    // --- Advanced
    p = pages_[1];
    data_ = radio_group(p, IDC_DATA, 8, 8, 102, 101, l.data, 3);
    phase_ = radio_group(p, IDC_PHASE, 8, 115, 102, 101, l.phase, 3);
    const int fx = 116, fy = 8;  // FilterGroupBox
    group(p, fx, fy, 224, 246, l.filter_group);
    enter(fx, fy, 224, 246);
    cfocus_chk_ = check(p, IDC_CFOCUS_CHK, fx + 16, fy + 21, l.w_cfocus, 17, l.cfocus);
    cfocus_ = trackbar(p, IDC_CFOCUS, fx + 25, fy + 44, 153, 40, 20, 15, true);
    cfocus_text_ = static_text(p, fx + 180, fy + 56, L"1.00");
    static_text(p, fx + 28, fy + 79, l.weak);
    static_text(p, fx + 164, fy + 79, l.strong);
    lpf_chk_ = check(p, IDC_LPF_CHK, fx + 16, fy + 100, l.w_filter_check, 17, l.lpf);
    lpf_ = trackbar(p, IDC_LPF, fx + 25, fy + 123, 153, 40, 20, 15, true);
    lpf_label_right_ = px(fx + 178 + 39);
    lpf_label_ = label(p, fx + 178, fy + 137, L"10.0kHz", nullptr, 39, 0, SS_RIGHT);
    hpf_chk_ = check(p, IDC_HPF_CHK, fx + 16, fy + 180, l.w_filter_check, 17, l.hpf);
    hpf_ = trackbar(p, IDC_HPF, fx + 25, fy + 203, 153, 40, 20, 15, true);
    hpf_label_right_ = px(fx + 180 + 31);
    hpf_label_ = label(p, fx + 180, fy + 217, L"100Hz", nullptr, 31, 0, SS_RIGHT);
    const int ox = 346, oy = 8;  // OvspGroupBox
    leave();
    group(p, ox, oy, 126, 105, l.ovsp_group);
    enter(ox, oy, 126, 105);
    ovsp_chk_ = check(p, IDC_OVSP_CHK, ox + 11, oy + 16, 137, 17, l.ovsp_check);
    const wchar_t* ovsp_items[] = {L"8", L"16", L"32", L"64", L"128"};
    ovsp_combo_ = combo(p, IDC_OVSP, ox + 31, oy + 39, 57, ovsp_items, 5, 3);
    static_text(p, ox + 94, oy + 43, l.ovsp_unit);  // empty in the en_US build
    if (l.ovsp_note_w)  // AutoSize = False
        static_text(p, ox + l.ovsp_note_x, oy + l.ovsp_note_y, l.ovsp_note, l.ovsp_note_w, l.ovsp_note_h, SS_CENTER);
    else
        static_text(p, ox + l.ovsp_note_x, oy + l.ovsp_note_y, l.ovsp_note);
    leave();
    const int bx = 346, by = l.pos_bsize_group;  // BsizeGroupBox
    group(p, bx, by, 126, 74, l.bsize_group);
    enter(bx, by, 126, 74);
    const wchar_t* bsize_items[] = {L"50", L"100", L"200", L"400", L"800"};
    bsize_combo_ = combo(p, IDC_BSIZE, bx + l.pos_bsize_combo.x, by + l.pos_bsize_combo.y, 57, bsize_items, 5, 3);
    label(p, bx + l.pos_bsize_unit.x, by + l.pos_bsize_unit.y, l.bsize_unit);
    leave();

    // --- Misc
    p = pages_[2];
    group(p, 8, 8, 225, 110, l.file_group);
    enter(8, 8, 225, 110);
    kname_ = check(p, IDC_KNAME, 24, 32, 153, 17, l.kname);
    vname_ = check(p, IDC_VNAME, 24, 63, 166, 17, l.vname);
    static_text(p, 8 + l.pos_vname_label, 88, l.vname_label);
    vname_edit_ = edit(p, IDC_VNAME_EDIT, 8 + l.pos_vname_edit, 86, 81, 20, 32);
    leave();
    // not in the original: the Utagoe Rip 4 separation
    group(p, 8, 126, 465, 98, l.v4_group);
    enter(8, 126, 465, 98);
    v4_chk_ = check(p, IDC_V4, 24, 148, 420, 17, l.v4_check);
    label(p, 26, 174, l.v4_note, nullptr, 430, 40);
    leave();
}

void SettingsForm::update_v4_enable() {
    bool on3 = !is_checked(v4_chk_);
    std::vector<HWND> off;
    for (const auto* g : {&intro_, &merge_, &level_, &data_, &phase_}) off.insert(off.end(), g->begin(), g->end());
    off.insert(off.end(), {adpt_auto_, adpt_manual_, adpt_edit_, adpt_updown_, klvl_, ovsp_chk_, ovsp_combo_,
                           bsize_combo_});
    for (HWND h : off) EnableWindow(h, on3);
}

void SettingsForm::show_page(int i) {
    for (int k = 0; k < 3; k++) ShowWindow(pages_[k], k == i ? SW_SHOW : SW_HIDE);
}

// TSetForm1.FormShow / ResetButtonClick
void SettingsForm::load(const Settings& c) {
    check_one(data_, c.proc_mode);
    check_one(merge_, c.merge_mode);
    set_check(ovsp_chk_, c.ovsp_flag);
    SetWindowTextW(ovsp_combo_, std::to_wstring(c.ovsp_mx).c_str());
    SetWindowTextW(bsize_combo_, std::to_wstring(c.blk_size).c_str());
    SendMessageW(adpt_updown_, UDM_SETPOS32, 0, c.adpt_num);
    SetWindowTextW(adpt_edit_, std::to_wstring(c.adpt_num).c_str());
    set_check(adpt_auto_, c.adpt_mode == ADPT_AUTO);
    set_check(adpt_manual_, c.adpt_mode != ADPT_AUTO);
    set_check(cfocus_chk_, c.cntr_flag);
    SendMessageW(cfocus_, TBM_SETPOS, TRUE, c.cntr_pos);
    check_one(intro_, c.intro_mode);
    check_one(level_, c.level_adpt);
    set_check(lpf_chk_, c.lpf_flag);
    SendMessageW(lpf_, TBM_SETPOS, TRUE, c.lpf_pos);
    set_check(hpf_chk_, c.hpf_flag);
    SendMessageW(hpf_, TBM_SETPOS, TRUE, c.hpf_pos);
    SendMessageW(kvol_, TBM_SETPOS, TRUE, c.kvol_pos);
    SendMessageW(klvl_, TBM_SETPOS, TRUE, c.klvl_pos);
    set_check(kname_, c.kname_flg);
    set_check(vname_, c.vname_flg);
    SetWindowTextW(vname_edit_, c.vname_txt.c_str());
    check_one(phase_, c.krk_phase);
    check_one(qty_, c.sound_qty);
    set_check(v4_chk_, c.v4);
    update_v4_enable();
    for (HWND t : {cfocus_, lpf_, hpf_, kvol_, klvl_}) update_labels(t);
}

// label formats of the TrackBarChange handlers
void SettingsForm::update_labels(HWND t) {
    Settings s;
    int pos = track_pos(t);
    if (t == kvol_) {
        s.kvol_pos = pos;
        set_static_text(kvol_text_, ui::format(L"%.1f", (double)s.kvol()));
    } else if (t == klvl_) {
        s.klvl_pos = pos;
        set_static_text(klvl_text_, ui::format(L"%.2f", (double)s.klvl()));
    } else if (t == cfocus_) {
        set_static_text(cfocus_text_, ui::format(L"%.2f", pos * 0.25 + 0.5));
    } else if (t == lpf_ || t == hpf_) {
        bool lp = t == lpf_;
        s.hpf_pos = pos;
        std::wstring text = lp ? ui::format(L"%.1fkHz", (pos * 800 + 2000) * 0.001) : ui::format(L"%dHz", s.hpf_hz());
        HWND lbl = lp ? lpf_label_ : hpf_label_;
        int right = lp ? lpf_label_right_ : hpf_label_right_;  // taRightJustify + AutoSize: keep the right edge
        int w = ui::text_width(font, text);
        RECT r;
        GetWindowRect(lbl, &r);
        MapWindowPoints(nullptr, GetParent(lbl), (POINT*)&r, 2);
        SetWindowPos(lbl, nullptr, right - w, r.top, w, r.bottom - r.top, SWP_NOZORDER);
        SetWindowTextW(lbl, text.c_str());
    }
}

// TSetForm1.FormClose with ModalResult = mrOK: store, convert, save
void SettingsForm::on_ok() {
    Settings& c = main_->cfg;
    c.proc_mode = checked(data_);
    c.merge_mode = checked(merge_);
    c.ovsp_flag = is_checked(ovsp_chk_);
    c.ovsp_mx = std::max(1, str_to_int_def(ui::window_text(ovsp_combo_), c.ovsp_mx));
    c.blk_size = std::max(50, str_to_int_def(ui::window_text(bsize_combo_), c.blk_size));
    c.adpt_mode = is_checked(adpt_auto_) ? ADPT_AUTO : ADPT_MANUAL;
    c.adpt_num = std::max(0, str_to_int_def(ui::window_text(adpt_edit_), c.adpt_num));
    c.cntr_flag = is_checked(cfocus_chk_);
    c.cntr_pos = track_pos(cfocus_);
    c.intro_mode = checked(intro_);
    c.level_adpt = checked(level_);
    c.lpf_flag = is_checked(lpf_chk_);
    c.hpf_flag = is_checked(hpf_chk_);
    c.lpf_pos = track_pos(lpf_);
    c.hpf_pos = track_pos(hpf_);
    c.kvol_pos = track_pos(kvol_);
    c.klvl_pos = track_pos(klvl_);
    c.kname_flg = is_checked(kname_);
    c.vname_flg = is_checked(vname_);
    c.vname_txt = sanitize_suffix(ui::window_text(vname_edit_)).substr(0, 32);
    if (c.vname_txt.empty()) c.vname_flg = false;
    c.krk_phase = checked(phase_);
    c.sound_qty = checked(qty_);
    c.v4 = is_checked(v4_chk_);
    main_->save_settings();
    close();
}

LRESULT SettingsForm::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: on_ok(); return 0;
        case IDCANCEL: close(); return 0;
        case IDC_V4:
            if (HIWORD(wp) == BN_CLICKED) update_v4_enable();
            return 0;
        case IDC_RESET: {  // defaults for everything but the Misc tab
            bool kn = is_checked(kname_), vn = is_checked(vname_), v4 = is_checked(v4_chk_);
            std::wstring vt = ui::window_text(vname_edit_);
            load(Settings());
            set_check(kname_, kn);
            set_check(vname_, vn);
            SetWindowTextW(vname_edit_, vt.c_str());
            set_check(v4_chk_, v4);
            update_v4_enable();
            return 0;
        }
        }
        break;
    case WM_HSCROLL:
        if (lp) update_labels((HWND)lp);
        return 0;
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == IDC_TAB && ((NMHDR*)lp)->code == TCN_SELCHANGE) {
            show_page((int)SendMessageW(tab_, TCM_GETCURSEL, 0, 0));
            return 0;
        }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CLOSE:
        close();
        return 0;
    }
    return default_handle(msg, wp, lp);
}

}  // namespace utagoe
