// TAboutForm: version box with the animated logo.
#include <windows.h>

#include "app.hpp"

namespace utagoe {

namespace {
const UINT_PTR TIMER_ANIM = 1;
const int SHAKE[16] = {150, 225, 244, 225, 145, 65, 15, -12, -20, -10, 8, 0, -4, 0, 2, 0};
}  // namespace

AboutForm::~AboutForm() {
    if (logo_) DeleteObject(logo_);
    if (offscreen_) DeleteObject(offscreen_);
    if (frame_) DeleteObject(frame_);
    if (big_font_) DeleteObject(big_font_);
}

void AboutForm::run() {
    const Lang& l = main_->L();
    font = main_->about_font;
    if (!create(l.about_title, 257, l.about_height, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, main_->hwnd))
        return;
    bevels = {{8, 8, 241, l.about_bevel, ui::BEVEL_BOX}};
    if (l.credits_w)
        label(hwnd, l.credits_x, l.credits_y, l.credits, nullptr, l.credits_w, l.credits_h, SS_RIGHT);
    else
        label(hwnd, l.credits_x, l.credits_y, l.credits);
    if (scaled()) {  // VerLabel has ParentFont = False
        big_font_ = make_font(L"ＭＳ Ｐゴシック", SHIFTJIS_CHARSET, ui::scaled_font_height(-16, scale_m, scale_d));
        label(hwnd, 166, 80, L"Version 4.2", big_font_);
    } else {
        label(hwnd, 166, 80, L"Version 4.2", main_->about_font_big);
    }
    img_ = {16, 17, 225, 64};  // LogoImg (Center = True)
    scale_rect(img_.x, img_.y, img_.w, img_.h);
    HWND ok = bitbtn(hwnd, IDOK, 88, l.about_ok_top, 75, 25, l.ok, main_->glyph_ok, -1, 4, BS_DEFPUSHBUTTON);

    // FormCreate: an off-screen bitmap (logo width + 500) with the logo at x = 150
    logo_ = (HBITMAP)LoadImageW(ui::hinst, L"BMP_LOGO", IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
    BITMAP bm;
    GetObjectW(logo_, sizeof bm, &bm);
    lw_ = bm.bmWidth, lh_ = bm.bmHeight;
    HDC screen = GetDC(nullptr);
    HDC src = CreateCompatibleDC(screen), dst = CreateCompatibleDC(screen);
    HGDIOBJ os = SelectObject(src, logo_);
    key_ = GetPixel(src, 0, lh_ - 1);  // transparent colour: bottom-left pixel
    offscreen_ = CreateCompatibleBitmap(screen, lw_ + 500, lh_);
    frame_ = CreateCompatibleBitmap(screen, lw_, lh_);
    HGDIOBJ od = SelectObject(dst, offscreen_);
    HBRUSH key_brush = CreateSolidBrush(key_);
    RECT all = {0, 0, lw_ + 500, lh_};
    FillRect(dst, &all, key_brush);
    BitBlt(dst, 150, 0, lw_, lh_, src, 0, 0, SRCCOPY);
    DeleteObject(key_brush);
    SelectObject(src, os);
    SelectObject(dst, od);
    DeleteDC(src);
    DeleteDC(dst);
    ReleaseDC(nullptr, screen);

    state_ = 0;
    SetTimer(hwnd, TIMER_ANIM, 50, nullptr);
    SetFocus(ok);
    show_modal(main_->hwnd);
}

// CopyRect(Rect(0, 0, W, H), offscreen, Rect(left, 0, left + width, H))
void AboutForm::draw_frame(int left, int width) {
    HDC screen = GetDC(nullptr);
    HDC src = CreateCompatibleDC(screen), dst = CreateCompatibleDC(screen);
    HGDIOBJ os = SelectObject(src, offscreen_), od = SelectObject(dst, frame_);
    HBRUSH key_brush = CreateSolidBrush(key_);
    RECT r = {0, 0, lw_, lh_};
    FillRect(dst, &r, key_brush);
    DeleteObject(key_brush);
    SetStretchBltMode(dst, COLORONCOLOR);
    // source columns outside the off-screen bitmap stay transparent
    int sl = left, sw = width, dl = 0, dw = lw_;
    if (sl < 0) {
        int cut = -sl;
        dl = (int)((long long)cut * lw_ / width);
        dw -= dl;
        sw -= cut;
        sl = 0;
    }
    if (sl + sw > lw_ + 500) {
        int cut = sl + sw - (lw_ + 500);
        int dcut = (int)((long long)cut * lw_ / width);
        sw -= cut;
        dw -= dcut;
    }
    if (sw > 0 && dw > 0) StretchBlt(dst, dl, 0, dw, lh_, src, sl, 0, sw, lh_, SRCCOPY);
    SelectObject(src, os);
    SelectObject(dst, od);
    DeleteDC(src);
    DeleteDC(dst);
    ReleaseDC(nullptr, screen);
    have_frame_ = true;
    RECT img = {img_.x, img_.y, img_.x + img_.w, img_.y + img_.h};
    InvalidateRect(hwnd, &img, TRUE);
}

void AboutForm::tick() {  // Timer1Timer (50 ms)
    switch (state_) {
    case 0: x_ = -106, count_ = 0, state_ = 1; break;
    case 1: if (++count_ > 5) state_ = 2; break;
    case 2:
        x_ += 80;
        draw_frame(x_, lw_);
        if (x_ >= 375) state_ = 3, count_ = 0;
        break;
    case 3: if (++count_ > 10) state_ = 4, x_ = -106; break;
    case 4:
        x_ += 64;
        draw_frame(x_, lw_);
        if (x_ >= 150) state_ = 5, idx_ = 0;
        break;
    case 5:
        draw_frame(150, lw_ + SHAKE[idx_]);
        if (++idx_ > 15) state_ = 6;
        break;
    default:
        KillTimer(hwnd, TIMER_ANIM);
    }
}

LRESULT AboutForm::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_bevels(dc);
        if (have_frame_) {
            // the 226x66 picture is centred in the 225x64 TImage, transparently
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, frame_);
            int save = SaveDC(dc);
            IntersectClipRect(dc, img_.x, img_.y, img_.x + img_.w, img_.y + img_.h);
            TransparentBlt(dc, img_.x + (img_.w - lw_) / 2, img_.y + (img_.h - lh_) / 2, lw_, lh_, mem, 0, 0, lw_, lh_, key_);
            RestoreDC(dc, save);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ANIM) tick();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            KillTimer(hwnd, TIMER_ANIM);
            close();
        }
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CLOSE:
        KillTimer(hwnd, TIMER_ANIM);
        close();
        return 0;
    }
    return default_handle(msg, wp, lp);
}

}  // namespace utagoe
