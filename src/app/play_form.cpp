// TPlayForm1: WAVE playback with a TMediaPlayer (MCI) and a position trackbar.
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

#include "app.hpp"

namespace utagoe {

namespace {
enum { IDC_MP_PLAY = 300, IDC_MP_PAUSE, IDC_MP_STOP, IDC_MP_PREV, IDC_TRACK };
enum { STOPPED = 1, PLAYING = 2, PAUSED = 5 };
const UINT_PTR TIMER_POS = 1;
int alias_counter = 0;
}  // namespace

bool PlayForm::mci(const std::wstring& cmd, std::wstring* result) {
    wchar_t buf[256] = L"";
    MCIERROR err = mciSendStringW(cmd.c_str(), buf, 256, nullptr);
    if (result) *result = buf;
    return err == 0;
}

long PlayForm::status_value(const wchar_t* item) {
    std::wstring r;
    mci(L"status " + alias_ + L" " + item, &r);
    return wcstol(r.c_str(), nullptr, 10);
}

void PlayForm::run() {
    alias_ = ui::format(L"utagoe%d", ++alias_counter);
    // WAV through the wave device, other formats through DirectShow (what Windows can play)
    bool wav = path_.size() >= 4 && !_wcsicmp(path_.c_str() + path_.size() - 4, L".wav");
    if (!mci(L"open \"" + path_ + L"\" type " + (wav ? L"waveaudio" : L"mpegvideo") + L" alias " + alias_)) return;
    mci(L"set " + alias_ + L" time format milliseconds");
    length_ = status_value(L"length");
    font = main_->font;
    if (!create(main_->L().play_title, 250, 133, WS_POPUP | WS_CAPTION, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                main_->hwnd)) {
        mci(L"close " + alias_);
        return;
    }
    bevels = {{0, 8, 249, 9, ui::BEVEL_TOP}, {0, 88, 249, 9, ui::BEVEL_BOTTOM}};
    // TMediaPlayer, VisibleButtons = [btPlay, btPause, btStop, btPrev]
    HWND first = media_player(hwnd, IDC_MP_PLAY, 68, 16, 30, main_->glyph_mp, 4);
    // TTrackBar defaults: TickMarks = tmBottomRight
    track_ = trackbar(hwnd, IDC_TRACK, 8, 56, 233, 33, (int)length_, 0, false);
    SendMessageW(track_, TBM_SETRANGEMAX, TRUE, length_);
    SendMessageW(track_, TBM_SETTICFREQ, length_ / 10 > 0 ? length_ / 10 : 1, 0);  // 0x409831
    SendMessageW(track_, TBM_SETPAGESIZE, 0, length_ / 40);
    SendMessageW(track_, TBM_SETLINESIZE, 0, length_ / 100);
    bitbtn(hwnd, IDOK, 88, 104, 75, 25, main_->L().ok, main_->glyph_ok, -1, 4, BS_DEFPUSHBUTTON);
    state_ = PLAYING;
    mci(L"play " + alias_);
    SetTimer(hwnd, TIMER_POS, 400, nullptr);
    SetFocus(first);
    show_modal(main_->hwnd);
}

void PlayForm::on_ok() {
    KillTimer(hwnd, TIMER_POS);
    mci(L"stop " + alias_);
    mci(L"close " + alias_);
    close();
}

LRESULT PlayForm::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MP_PLAY:
            if (status_value(L"position") >= length_) mci(L"seek " + alias_ + L" to start");
            mci(L"play " + alias_);
            state_ = PLAYING;
            return 0;
        case IDC_MP_PAUSE:
            if (state_ == PLAYING) mci(L"pause " + alias_), state_ = PAUSED;
            else if (state_ == PAUSED) mci(L"resume " + alias_), state_ = PLAYING;
            return 0;
        case IDC_MP_STOP:
            mci(L"stop " + alias_);
            state_ = STOPPED;
            return 0;
        case IDC_MP_PREV:
            mci(L"seek " + alias_ + L" to start");
            state_ = STOPPED;
            return 0;
        case IDOK:
        case IDCANCEL:
            on_ok();
            return 0;
        }
        break;
    case WM_TIMER: {  // Timer1Timer
        long pos = status_value(L"position");
        timer_update_ = true;
        SendMessageW(track_, TBM_SETPOS, TRUE, pos);
        timer_update_ = false;
        std::wstring mode;
        mci(L"status " + alias_ + L" mode", &mode);
        if (mode == L"stopped" && pos >= length_) state_ = STOPPED;
        return 0;
    }
    case WM_HSCROLL:  // TrackBar1Change: seek, and keep playing if we were
        if ((HWND)lp == track_ && !timer_update_ && LOWORD(wp) != TB_ENDTRACK) {
            long pos = (long)SendMessageW(track_, TBM_GETPOS, 0, 0);
            mci(ui::format(L"seek %ls to %ld", alias_.c_str(), pos));
            if (state_ == PLAYING) mci(L"play " + alias_);
        }
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CLOSE:
        on_ok();
        return 0;
    }
    return default_handle(msg, wp, lp);
}

}  // namespace utagoe
