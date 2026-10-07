// The original program's four forms: TForm1 (main), TSetForm1, TPlayForm1, TAboutForm.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "engine/engine.hpp"
#include "lang.hpp"
#include "ui.hpp"

namespace utagoe {

extern const wchar_t* const APP_TITLE;

HFONT make_font(const wchar_t* face, unsigned char charset, int height);

class MainForm : public ui::Form {
public:
    MainForm(const std::wstring& ini_path, const std::wstring& language);
    ~MainForm() override;
    bool create_window();

    const Lang& L() const { return *lang_; }
    Settings cfg;
    std::wstring ini_path;
    void save_settings();
    void warn(const std::wstring& text, HWND owner = nullptr);
    bool confirm(const std::wstring& text, HWND owner = nullptr);
    ui::Glyph glyph_ok, glyph_cancel;
    ui::Glyph glyph_mp[4];
    HFONT about_font = nullptr, about_font_big = nullptr;

protected:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void build();
    bool load_info(const std::wstring& path, HWND label);
    void auto_search(const std::wstring& path);
    void on_browse(int which);
    void on_enter(int edit_id);
    void on_drop(HDROP drop);
    void on_play(HWND edit);
    void on_help();
    void on_start();
    bool close_query();
    void work(std::wstring p1, std::wstring p2, std::wstring p3, bool single, Settings cfg);
    void finish(int error);
    void set_status(int status);
    void open_log(const std::wstring& path);
    void write_log(const std::wstring& line);
    void log_time();
    void close_log();
    static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

    const Lang* lang_;
    HWND edit1_ = nullptr, edit2_ = nullptr, edit3_ = nullptr, wform1_ = nullptr, wform2_ = nullptr;
    HWND info_ = nullptr, progress_ = nullptr, start_ = nullptr;
    HFONT start_font_ = nullptr;
    std::vector<HWND> lockable_;
    bool busy_ = false, debug_ = false, close_pending_ = false;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    std::wstring read_failed_;  // the input the worker could not decode
    AnalysisCache cache_;
    FILE* log_ = nullptr;
    DWORD log_start_ = 0;
    std::wstring status_text_;
    int status_count_ = 0;
};

class SettingsForm : public ui::Form {
public:
    explicit SettingsForm(MainForm* main) : main_(main) {}
    void run();

protected:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void build();
    void load(const Settings& c);
    void update_labels(HWND track);
    void on_ok();
    void show_page(int i);
    void update_v4_enable();  // grey out the 3.0-only settings while v4 is on
    MainForm* main_;
    HWND tab_ = nullptr, pages_[3] = {};
    std::vector<HWND> intro_, merge_, level_, qty_, data_, phase_;
    HWND adpt_auto_ = nullptr, adpt_manual_ = nullptr, adpt_edit_ = nullptr, adpt_updown_ = nullptr;
    HWND kvol_ = nullptr, kvol_text_ = nullptr, klvl_ = nullptr, klvl_text_ = nullptr;
    HWND cfocus_chk_ = nullptr, cfocus_ = nullptr, cfocus_text_ = nullptr;
    HWND lpf_chk_ = nullptr, lpf_ = nullptr, lpf_label_ = nullptr, hpf_chk_ = nullptr, hpf_ = nullptr, hpf_label_ = nullptr;
    HWND ovsp_chk_ = nullptr, ovsp_combo_ = nullptr, bsize_combo_ = nullptr;
    HWND kname_ = nullptr, vname_ = nullptr, vname_edit_ = nullptr;
    HWND v4_chk_ = nullptr;
    int lpf_label_right_ = 0, hpf_label_right_ = 0;
};

class PlayForm : public ui::Form {
public:
    PlayForm(MainForm* main, const std::wstring& path) : main_(main), path_(path) {}
    void run();

protected:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    bool mci(const std::wstring& cmd, std::wstring* result = nullptr);
    long status_value(const wchar_t* item);
    void on_ok();
    MainForm* main_;
    std::wstring path_, alias_;
    HWND track_ = nullptr;
    long length_ = 0;
    int state_ = 2;           // 1 stopped, 2 playing, 5 paused (TPlayForm1)
    bool timer_update_ = false;
};

class AboutForm : public ui::Form {
public:
    explicit AboutForm(MainForm* main) : main_(main) {}
    ~AboutForm() override;
    void run();

protected:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void tick();
    void draw_frame(int left, int width);
    MainForm* main_;
    HBITMAP logo_ = nullptr, offscreen_ = nullptr, frame_ = nullptr;
    COLORREF key_ = 0;
    HFONT big_font_ = nullptr;
    struct { int x, y, w, h; } img_ = {};
    int lw_ = 0, lh_ = 0;
    int state_ = 0, x_ = 0, count_ = 0, idx_ = 0;
    bool have_frame_ = false;
};

}  // namespace utagoe
