// TForm1: the main window.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <ctime>
#include <new>

#include "app.hpp"
#include "engine/audio_io.hpp"
#include "logic.hpp"

namespace utagoe {

const wchar_t* const APP_TITLE = L"歌声りっぷ";

namespace {

enum {
    IDC_EDIT1 = 101, IDC_EDIT2, IDC_EDIT3,
    IDC_BROWSE1 = 111, IDC_BROWSE2, IDC_BROWSE3,
    IDC_PLAY1 = 121, IDC_PLAY2, IDC_PLAY3,
    IDC_START = 130, IDC_SETTINGS, IDC_HELP_BTN, IDC_ABOUT, IDC_QUIT, IDC_DBG,
};
enum { WM_PROGRESS = WM_APP + 1, WM_STATUS, WM_DONE };
enum { DONE_OK, DONE_NO_MEMORY, DONE_WRITE_ERROR, DONE_READ_ERROR };
const UINT_PTR TIMER_STATUS = 1;

// the matched instrumental's file next to the vocal: <vocal>_inst.wav (4.3)
std::wstring inst_path(const std::wstring& vocal) {
    size_t dot = vocal.rfind(L'.'), slash = vocal.find_last_of(L"\\/");
    std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? vocal.substr(0, dot) : vocal;
    return stem + L"_inst.wav";
}

std::wstring file_name(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/:");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

std::wstring change_ext(const std::wstring& path, const wchar_t* ext) {
    size_t s = path.find_last_of(L"\\/:"), dot = path.rfind(L'.');
    if (dot != std::wstring::npos && (s == std::wstring::npos || dot > s)) return path.substr(0, dot) + ext;
    return path + ext;
}

std::wstring upper_ext(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/:"), dot = path.rfind(L'.');
    if (dot == std::wstring::npos || (s != std::wstring::npos && dot < s)) return L"";
    std::wstring e = path.substr(dot);
    CharUpperBuffW(&e[0], (DWORD)e.size());
    return e;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

HFONT make_font(const wchar_t* face, unsigned char charset, int height) {
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, charset, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

MainForm::MainForm(const std::wstring& ini, const std::wstring& language) {
    ini_path = ini;
    if (ini_path.empty()) {
        wchar_t dir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir)))
            ini_path = std::wstring(dir) + L"\\UtagoeRip\\utagoe.ini";
    }
    if (!cfg.load_ini(ini_path)) {  // an INI left behind by the original program
        size_t s = ini_path.find_last_of(L"\\/");
        if (s != std::wstring::npos) {
            std::wstring dir = ini_path.substr(0, s + 1);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"*.ini").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                cfg.load_ini(dir + fd.cFileName);
                FindClose(h);
            }
        }
    }
    lang_ = &pick_language(language.empty() ? cfg.language : language);
    font = make_font(L().font, L().charset, -12);
    about_font = make_font(L"ＭＳ Ｐゴシック", SHIFTJIS_CHARSET, -12);  // TAboutForm keeps it in both builds
    about_font_big = make_font(L"ＭＳ Ｐゴシック", SHIFTJIS_CHARSET, -16);
}

MainForm::~MainForm() {
    if (worker_.joinable()) {
        cancel_ = true;
        worker_.join();
    }
    close_log();
}

bool MainForm::create_window() {
    if (!create(APP_TITLE, 569, 294, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, WS_EX_CONTROLPARENT,
                nullptr))
        return false;
    build();
    DragAcceptFiles(hwnd, TRUE);
    show_focus_cues();
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(edit1_);
    return true;
}

void MainForm::build() {
    const Lang& l = L();
    glyph_ok = ui::load_glyph(L"BMP_OK");
    glyph_cancel = ui::load_glyph(L"BMP_CANCEL");
    const wchar_t* mp[4][2] = {{L"BMP_MP_PLAY", L"BMP_MP_PLAY_D"}, {L"BMP_MP_PAUSE", L"BMP_MP_PAUSE_D"}, {L"BMP_MP_STOP", L"BMP_MP_STOP_D"},
                               {L"BMP_MP_PREV", L"BMP_MP_PREV_D"}};
    for (int i = 0; i < 4; i++) glyph_mp[i] = ui::load_glyph(mp[i][0], mp[i][1]);
    ui::Glyph open = ui::load_glyph(L"BMP_OPEN"), play = ui::load_glyph(L"BMP_PLAY");

    bevels = {{8, 8, 553, 257, ui::BEVEL_BOX}, {16, 168, 433, 9, ui::BEVEL_TOP}, {456, 16, 9, 241, ui::BEVEL_LEFT},
              {458, 270, 103, 17, ui::BEVEL_BOX}};
    label(hwnd, 16, 24, l.label1);
    label(hwnd, 16, 96, l.label2);
    label(hwnd, 16, 184, l.label3);
    wform1_ = label(hwnd, 196, 24, L"", nullptr, 250);
    wform2_ = label(hwnd, 196, 96, L"", nullptr, 250);
    info_ = label(hwnd, 464, 272, L"", nullptr, 95);

    // tab order as in the original
    edit1_ = edit(hwnd, IDC_EDIT1, 24, 48, 337, 20, 260);
    HWND b1 = bitbtn(hwnd, IDC_BROWSE1, 376, 46, 25, 25, L"", open);
    HWND p1 = bitbtn(hwnd, IDC_PLAY1, 416, 46, 25, 25, L"", play);
    edit2_ = edit(hwnd, IDC_EDIT2, 24, 120, 337, 20, 260);
    HWND b2 = bitbtn(hwnd, IDC_BROWSE2, 376, 118, 25, 25, L"", open);
    HWND p2 = bitbtn(hwnd, IDC_PLAY2, 416, 118, 25, 25, L"", play);
    edit3_ = edit(hwnd, IDC_EDIT3, 24, 208, 337, 20, 260);
    HWND b3 = bitbtn(hwnd, IDC_BROWSE3, 376, 206, 25, 25, L"", open);
    HWND p3 = bitbtn(hwnd, IDC_PLAY3, 416, 206, 25, 25, L"", play);
    start_ = bitbtn(hwnd, IDC_START, 464, 16, 89, 57, l.start, ui::load_glyph(L"BMP_NOTE"));
    if (scaled()) {  // StartBtn has ParentFont = False, so its font is scaled with the form
        start_font_ = make_font(l.font, l.charset, ui::scaled_font_height(-12, scale_m, scale_d));
        set_bitbtn_font(start_, start_font_);
    }
    HWND set = bitbtn(hwnd, IDC_SETTINGS, 464, 152, 89, 25, l.settings_btn, ui::load_glyph(L"BMP_SETTINGS"), 2, 6);
    HWND help = bitbtn(hwnd, IDC_HELP_BTN, 464, 192, 89, 25, l.help_btn, ui::load_glyph(L"BMP_HELP"), 1);
    HWND about = bitbtn(hwnd, IDC_ABOUT, 464, 232, 25, 25, L"", ui::load_glyph(L"BMP_ABOUT"), 1);
    bitbtn(hwnd, IDC_QUIT, 490, 232, 63, 25, l.quit, ui::load_glyph(L"BMP_QUIT"), -1, 5);
    progress_ = child(PROGRESS_CLASSW, L"", 0, 0, hwnd, -1, 8, 270, 447, 17);
    SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
    child(L"STATIC", L"", SS_NOTIFY, 0, hwnd, IDC_DBG, 9, 256, 8, 8);  // DbgPanel

    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, ui::hinst, nullptr);
    TTTOOLINFOW ti = {sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hwnd;
    ti.uId = (UINT_PTR)about;
    ti.lpszText = (LPWSTR)l.about_hint;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(tip, WM_SETFONT, (WPARAM)font, FALSE);

    for (HWND e : {edit1_, edit2_, edit3_}) SetWindowSubclass(e, edit_proc, 1, (DWORD_PTR)this);
    lockable_ = {set, p1, p2, p3, b1, b2, b3, about, edit1_, edit2_, edit3_, help};
}

void MainForm::warn(const std::wstring& text, HWND owner) {
    MessageBoxW(owner ? owner : hwnd, text.c_str(), L().warning, MB_OK | MB_ICONWARNING);
}

bool MainForm::confirm(const std::wstring& text, HWND owner) {
    return MessageBoxW(owner ? owner : hwnd, text.c_str(), L().confirm, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void MainForm::save_settings() { cfg.save_ini(ini_path); }

// 0x406fd4: show the format of `path`; returns true on error
bool MainForm::load_info(const std::wstring& path, HWND label) {
    AudioInfo info;
    int err = audio_info(path, &info);
    if (err) {
        const wchar_t* f = err == WAV_OPEN_FAILED ? L().cannot_open : err == WAV_BAD_FILE ? L().bad_file : L().bad_format;
        warn(ui::format(f, file_name(path).c_str()));
        SetWindowTextW(label, L"");
        return true;
    }
    std::wstring ch = info.channels == 1 ? L().mono : info.channels == 2 ? L().stereo
                                                     : ui::format(L().channels_n, info.channels);
    // a 16-bit WAV reads as in the original; anything else names its format
    if (info.codec == L"PCM" && info.bits)
        SetWindowTextW(label, ui::format(L().info, info.rate * 0.001, info.bits, ch.c_str()).c_str());
    else
        SetWindowTextW(label, ui::format(L().info_codec, info.rate * 0.001,
                                         (info.bits ? info.codec + L" " + std::to_wstring(info.bits) + L"-bit"
                                                    : info.codec).c_str(), ch.c_str()).c_str());
    return false;
}

void MainForm::auto_search(const std::wstring& path) {  // 0x4058a8
    std::wstring inst, out;
    auto_names(path, cfg.kname_flg, cfg.vname_flg, cfg.vname_txt, &inst, &out);
    if (!inst.empty()) {
        SetWindowTextW(edit2_, inst.c_str());
        load_info(inst, wform2_);
    }
    if (!out.empty()) SetWindowTextW(edit3_, out.c_str());
}

void MainForm::on_browse(int which) {
    std::wstring filter;
    if (which == 3) {
        filter = std::wstring(L().wave_files) + L'\0' + L"*.WAV" + L'\0';
    } else {
        std::wstring pat;
        for (const wchar_t* const* e = AUDIO_EXTENSIONS; *e; e++) pat += (pat.empty() ? L"*" : L";*") + std::wstring(*e);
        filter = std::wstring(L().audio_files) + L'\0' + pat + L'\0' + L().all_files + L'\0' + L"*.*" + L'\0';
    }
    filter += L'\0';
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = hwnd;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_HIDEREADONLY | OFN_EXPLORER | OFN_ENABLESIZING;
    if (which == 3) {
        of.lpstrDefExt = L"wav";
        if (!GetSaveFileNameW(&of)) return;
        SetWindowTextW(edit3_, file);
        return;
    }
    if (!GetOpenFileNameW(&of)) return;
    if (which == 1) {
        SetWindowTextW(edit1_, file);
        if (!load_info(file, wform1_)) auto_search(file);
    } else {
        SetWindowTextW(edit2_, file);
        load_info(file, wform2_);
    }
}

void MainForm::on_enter(int id) {
    if (id == IDC_EDIT2) {
        std::wstring p = ui::window_text(edit2_);
        if (!p.empty()) load_info(p, wform2_);
        return;
    }
    // Edit1KeyPress, which the original also uses for the output box
    std::wstring p = ui::window_text(edit1_);
    if (!p.empty() && !load_info(p, wform1_)) auto_search(p);
}

void MainForm::on_drop(HDROP drop) {  // 0x406d58
    POINT pt;
    DragQueryPoint(drop, &pt);
    wchar_t file[MAX_PATH] = L"";
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (n) DragQueryFileW(drop, 0, file, MAX_PATH);
    DragFinish(drop);
    if (busy_ || !n) return;
    int zone = 0;
    if (pt.x >= 8 && pt.x < 457) zone = pt.y >= 8 && pt.y < 90 ? 1 : pt.y >= 90 && pt.y < 169 ? 2 : pt.y >= 169 && pt.y < 265 ? 3 : 0;
    if (!zone) return;
    if (zone != 3 && !is_audio_extension(upper_ext(file))) {
        warn(L().drop_wave);
        return;
    }
    if (zone == 1) {
        SetWindowTextW(edit1_, file);
        if (!load_info(file, wform1_)) auto_search(file);
    } else if (zone == 2) {
        SetWindowTextW(edit2_, file);
        load_info(file, wform2_);
    } else {
        SetWindowTextW(edit3_, file);
    }
}

void MainForm::on_play(HWND edit) {
    std::wstring p = ui::window_text(edit);
    if (p.empty() || !is_audio_extension(upper_ext(p)) || !exists(p)) return;
    PlayForm(this, p).run();
}

void MainForm::on_help() {
    HRSRC r = FindResourceW(ui::hinst, L().help_resource, (LPCWSTR)RT_RCDATA);
    int code = 2;
    if (r) {
        HGLOBAL g = LoadResource(ui::hinst, r);
        DWORD size = SizeofResource(ui::hinst, r);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"UtagoeRip";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\" + L().help_file;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            fwrite(LockResource(g), 1, size, f);
            fclose(f);
            code = (int)(INT_PTR)ShellExecuteW(hwnd, L"open", path.c_str(), L"", L"", SW_SHOWNORMAL);
        }
    }
    if (code > 32) return;
    if (code == 2 || code == 3) warn(L().help_missing);
    else if (code == SE_ERR_NOASSOC) warn(L().help_reader);
    else warn(L().help_error);
}

bool MainForm::close_query() {
    if (!busy_) return true;
    if (!confirm(L().confirm_abort)) return false;
    cancel_ = true;
    close_pending_ = true;
    return false;  // closes when the worker has stopped
}

// ---------------------------------------------------------------- processing (0x407718)

void MainForm::on_start() {
    const Lang& l = L();
    if (busy_) {
        if (confirm(l.confirm_abort)) cancel_ = true;
        return;
    }
    std::wstring p1 = ui::window_text(edit1_), p2 = ui::window_text(edit2_), p3 = ui::window_text(edit3_);
    if (p1.empty() || p2.empty() || p3.empty()) return warn(l.need_names);
    if (p3 == p1 || p3 == p2) return warn(l.same_file);
    bool single = p2 == p1;  // the same file twice: process the original on its own
    if (load_info(p1, wform1_) || load_info(p2, wform2_)) return;
    p3 = change_ext(p3, L".wav");
    SetWindowTextW(edit3_, p3.c_str());
    std::wstring name3 = file_name(p3);
    if (exists(p3) && !confirm(ui::format(l.overwrite, name3.c_str()))) return;
    std::wstring p_inst = inst_path(p3);
    if (cfg.v4 && cfg.v4_inst && !single) {
        if (_wcsicmp(p_inst.c_str(), p1.c_str()) == 0 || _wcsicmp(p_inst.c_str(), p2.c_str()) == 0)
            return warn(l.same_file);
        if (exists(p_inst)) {
            if (!confirm(ui::format(l.overwrite, file_name(p_inst).c_str()))) return;
            // a run that saves none (3.0's processing under 8 s, cancelled) leaves no stale one
            DeleteFileW(p_inst.c_str());
        }
    }
    FILE* f = _wfopen(p3.c_str(), L"wb");
    if (!f) return warn(ui::format(l.create_failed, name3.c_str()));
    fclose(f);

    open_log(change_ext(p1, L".txt"));
    busy_ = true;
    cancel_ = false;
    set_caption(start_, l.abort);
    for (HWND w : lockable_) EnableWindow(w, FALSE);
    SendMessageW(progress_, PBM_SETPOS, 0, 0);
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread(&MainForm::work, this, p1, p2, p3, single, cfg);
}

void MainForm::work(std::wstring p1, std::wstring p2, std::wstring p3, bool single, Settings c) {
    int result = DONE_OK;
    try {
        Audio orig, inst;
        if (!load_audio(p1, 0, 0, &orig) || (!single && !load_audio(p2, orig.rate, orig.channels, &inst))) {
            read_failed_ = file_name(orig.data.empty() ? p1 : p2);  // finish() reads it after the join
            result = DONE_READ_ERROR;
        } else {
            Callbacks cb;
            cb.log = [this](const std::wstring& s) { write_log(log_line(L(), s)); };
            cb.progress = [this](double f) { PostMessageW(hwnd, WM_PROGRESS, (WPARAM)(f * 100), 0); };
            cb.status = [this](Status s) { PostMessageW(hwnd, WM_STATUS, (WPARAM)s, 0); };
            cb.cancel = [this] { return cancel_.load(); };
            cb.timestamp = [this] { log_time(); };
            UtagoeRip rip(orig, single ? nullptr : &inst, c, cb, false, &cache_, p1, p2);
            Audio out = rip.run();
            if (!write_wav(p3, out)) result = DONE_WRITE_ERROR;
            else if (!rip.instrumental().data.empty() && !write_wav(inst_path(p3), rip.instrumental()))
                result = DONE_WRITE_ERROR;
        }
    } catch (std::bad_alloc&) {
        result = DONE_NO_MEMORY;
    } catch (...) {
        result = DONE_WRITE_ERROR;
    }
    PostMessageW(hwnd, WM_DONE, (WPARAM)result, 0);
}

void MainForm::finish(int error) {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    set_status(STATUS_NONE);
    SendMessageW(progress_, PBM_SETPOS, 0, 0);
    SetWindowTextW(info_, L"");
    SetWindowTextW(hwnd, APP_TITLE);
    set_caption(start_, L().start);
    for (HWND w : lockable_) EnableWindow(w, TRUE);
    close_log();
    if (error == DONE_NO_MEMORY) warn(L().no_memory);
    else if (error == DONE_WRITE_ERROR) warn(L().write_error);
    else if (error == DONE_READ_ERROR) warn(ui::format(L().bad_file, read_failed_.c_str()));
    if (close_pending_) DestroyWindow(hwnd);
}

// 0x412494 / 0x412664: status text followed by an arrow that steps right every second
void MainForm::set_status(int status) {
    KillTimer(hwnd, TIMER_STATUS);
    if (status == STATUS_NONE) {
        status_text_.clear();
        SetWindowTextW(info_, L"");
        return;
    }
    status_text_ = status == STATUS_ANALYZING ? L().analyzing : L().preparing;
    status_count_ = 0;
    SetTimer(hwnd, TIMER_STATUS, 1000, nullptr);
}

// ---------------------------------------------------------------- hidden debug log

void MainForm::open_log(const std::wstring& path) {
    log_ = nullptr;
    if (!debug_) return;
    log_ = _wfopen(path.c_str(), L"wb");
    log_start_ = GetTickCount();
    log_time();
}

void MainForm::write_log(const std::wstring& line) {
    if (!log_) return;
    std::wstring s = line + L"\r\n";
    int n = WideCharToMultiByte(932, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string b(n, '\0');
    WideCharToMultiByte(932, 0, s.c_str(), (int)s.size(), &b[0], n, nullptr, nullptr);
    fwrite(b.data(), 1, b.size(), log_);
    fflush(log_);
}

void MainForm::log_time() {
    if (!log_) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring now = ui::format(L"%04d/%02d/%02d %d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    write_log(ui::format(L().elapsed, now.c_str(), (int)((GetTickCount() - log_start_) / 1000)));
}

void MainForm::close_log() {
    if (!log_) return;
    log_time();
    fclose(log_);
    log_ = nullptr;
}

// ---------------------------------------------------------------- messages

LRESULT CALLBACK MainForm::edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    MainForm* f = (MainForm*)ref;
    if (msg == WM_GETDLGCODE && lp && ((MSG*)lp)->message == WM_KEYDOWN && ((MSG*)lp)->wParam == VK_RETURN)
        return DLGC_WANTALLKEYS | DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        f->on_enter(GetDlgCtrlID(h));
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT MainForm::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE1: on_browse(1); return 0;
        case IDC_BROWSE2: on_browse(2); return 0;
        case IDC_BROWSE3: on_browse(3); return 0;
        case IDC_PLAY1: on_play(edit1_); return 0;
        case IDC_PLAY2: on_play(edit2_); return 0;
        case IDC_PLAY3: on_play(edit3_); return 0;
        case IDC_START: on_start(); return 0;
        case IDC_SETTINGS: SettingsForm(this).run(); return 0;
        case IDC_HELP_BTN: on_help(); return 0;
        case IDC_ABOUT: AboutForm(this).run(); return 0;
        case IDC_QUIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDC_DBG:
            if (HIWORD(wp) == STN_DBLCLK) {
                debug_ = !debug_;
                warn(ui::format(L().debug, debug_ ? L"ON" : L"OFF"));
            }
            return 0;
        }
        break;
    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;
    case WM_PROGRESS: {
        int pct = (int)wp;
        SendMessageW(progress_, PBM_SETPOS, pct, 0);
        SetWindowTextW(info_, ui::format(L"%d %%", pct).c_str());
        SetWindowTextW(hwnd, ui::format(L"%d%%-%ls", pct, APP_TITLE).c_str());
        return 0;
    }
    case WM_STATUS:
        set_status((int)wp);
        return 0;
    case WM_DONE:
        finish((int)wp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_STATUS && !status_text_.empty()) {
            SetWindowTextW(info_, (status_text_ + std::wstring(status_count_, L' ') + L"->").c_str());
            status_count_ = status_count_ >= 4 ? 0 : status_count_ + 1;
        }
        return 0;
    case WM_CLOSE:
        if (close_query()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return default_handle(msg, wp, lp);
}

}  // namespace utagoe
