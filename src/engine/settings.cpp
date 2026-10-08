#include "settings.hpp"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace utagoe {

namespace {

const unsigned CP_SJIS = 932;

std::wstring decode(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_SJIS, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_SJIS, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string encode(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_SJIS, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_SJIS, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct Key {
    const wchar_t* name;
    int Settings::*i;
    bool Settings::*b;
    std::wstring Settings::*s;
};

// key order of TSetForm1's INI writer (0x40ac8c)
const Key KEYS[] = {
    {L"ProcMode", &Settings::proc_mode, nullptr, nullptr},
    {L"MergeMode", &Settings::merge_mode, nullptr, nullptr},
    {L"OvspFlg", nullptr, &Settings::ovsp_flag, nullptr},
    {L"OvspMx", &Settings::ovsp_mx, nullptr, nullptr},
    {L"BlkSize", &Settings::blk_size, nullptr, nullptr},
    {L"AdptMode", &Settings::adpt_mode, nullptr, nullptr},
    {L"AdptNum", &Settings::adpt_num, nullptr, nullptr},
    {L"CntrFlg", nullptr, &Settings::cntr_flag, nullptr},
    {L"CntrPos", &Settings::cntr_pos, nullptr, nullptr},
    {L"IntroMode", &Settings::intro_mode, nullptr, nullptr},
    {L"LevelAdpt", &Settings::level_adpt, nullptr, nullptr},
    {L"LPFFlg", nullptr, &Settings::lpf_flag, nullptr},
    {L"LPFPos", &Settings::lpf_pos, nullptr, nullptr},
    {L"HPFFlg", nullptr, &Settings::hpf_flag, nullptr},
    {L"HPFPos", &Settings::hpf_pos, nullptr, nullptr},
    {L"KvolPos", &Settings::kvol_pos, nullptr, nullptr},
    {L"KlvlPos", &Settings::klvl_pos, nullptr, nullptr},
    {L"KnameFlg", nullptr, &Settings::kname_flg, nullptr},
    {L"VnameFlg", nullptr, &Settings::vname_flg, nullptr},
    {L"VnameTxt", nullptr, nullptr, &Settings::vname_txt},
    {L"KrkPhase", &Settings::krk_phase, nullptr, nullptr},
    {L"SoundQty", &Settings::sound_qty, nullptr, nullptr},
    {L"Language", nullptr, nullptr, &Settings::language},
    {L"V4Engine", nullptr, &Settings::v4, nullptr},
    {L"V4SaveInst", nullptr, &Settings::v4_inst, nullptr},
};

}  // namespace

bool Settings::load_ini(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.append(buf, n);
    fclose(f);
    std::wstring text = decode(raw);

    bool in_section = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = trim(line);
        if (t.empty() || t[0] == L';') continue;
        if (t[0] == L'[') {
            in_section = _wcsicmp(t.c_str(), L"[V30_Option]") == 0;
            continue;
        }
        if (!in_section) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = trim(line.substr(0, eq));
        std::wstring val = line.substr(eq + 1);
        for (const Key& k : KEYS) {
            if (_wcsicmp(key.c_str(), k.name) != 0) continue;
            std::wstring v = trim(val);
            if (k.s) {
                this->*k.s = val;
            } else if (k.b) {
                this->*k.b = !(v.empty() || v == L"0" || _wcsicmp(v.c_str(), L"false") == 0);
            } else {
                wchar_t* end = nullptr;
                long x = wcstol(v.c_str(), &end, 10);
                if (end && end != v.c_str()) this->*k.i = (int)x;
            }
        }
    }
    return true;
}

bool Settings::save_ini(const std::wstring& path) const {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::wstring text = L"[V30_Option]\r\n";
    for (const Key& k : KEYS) {
        text += k.name;
        text += L"=";
        if (k.s) text += this->*k.s;
        else if (k.b) text += (this->*k.b) ? L"1" : L"0";
        else text += std::to_wstring(this->*k.i);
        text += L"\r\n";
    }
    std::string bytes = encode(text);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}

}  // namespace utagoe
