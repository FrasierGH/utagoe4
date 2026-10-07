#include "logic.hpp"

#include <windows.h>

#include "engine/audio_io.hpp"

#include <algorithm>
#include <climits>
#include <vector>

namespace utagoe {

namespace {

// Keywords the original looks for in instrumental file names (0x406820).
const wchar_t* const KEYWORDS[] = {
    L"inst", L"karaoke", L"without", L"vocalless", L"vocal less", L"vocal-less", L"off vocal", L"less vocal",
    L"music only", L"インスト", L"カラオケ", L"オフボーカル", L"オフヴォーカル", L"ｲﾝｽﾄ", L"ｶﾗｵｹ", L"ｵﾌﾎﾞｰｶﾙ",
    L"ｵﾌｳﾞｫｰｶﾙ", L"からおけ"};

struct Cand {
    std::wstring name;
    long long diff;
};

std::wstring upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

bool same(const std::wstring& a, const std::wstring& b) {
    return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1, b.c_str(), -1) == CSTR_EQUAL;
}

long long file_size(const std::wstring& path) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return (long long)fd.nFileSizeLow | (long long)fd.nFileSizeHigh << 32;
}

// FindFirst(dir + pattern) like the original: Windows wildcard matching, files only.
std::vector<Cand> find(const std::wstring& dir, const std::wstring& pattern, long long ref_size) {
    std::vector<Cand> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        long long size = (long long)fd.nFileSizeLow | (long long)fd.nFileSizeHigh << 32;
        out.push_back({fd.cFileName, std::llabs(size - ref_size)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// find() over every audio extension the program reads (4.2; the original looked for
// *.wav only). One file can match two patterns (*.wav also finds .wave), so names are
// kept once.
std::vector<Cand> find_audio(const std::wstring& dir, const std::wstring& pat, long long ref_size) {
    std::vector<Cand> out;
    for (const wchar_t* const* e = AUDIO_EXTENSIONS; *e; e++)
        for (Cand& c : find(dir, pat + L"*" + *e, ref_size))
            if (std::none_of(out.begin(), out.end(), [&](const Cand& o) { return same(o.name, c.name); }))
                out.push_back(c);
    return out;
}

// 0x406820: prefer a keyword match, then the closest file size.
std::wstring select(std::vector<Cand> cands, const std::wstring& orig_name, const std::wstring& exclude, bool vname_flg) {
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1) == CSTR_LESS_THAN;
    });
    cands.erase(std::remove_if(cands.begin(), cands.end(), [&](const Cand& c) {
        return same(c.name, orig_name) || (vname_flg && same(c.name, exclude));
    }), cands.end());
    long long best = INT_MAX;
    std::wstring result;
    for (const Cand& c : cands) {
        std::wstring up = upper(c.name);
        for (const wchar_t* kw : KEYWORDS) {
            if (up.find(upper(kw)) != std::wstring::npos) {
                if (c.diff < best) best = c.diff, result = c.name;
                break;
            }
        }
    }
    if (result.empty())
        for (const Cand& c : cands)
            if (c.diff < best) best = c.diff, result = c.name;
    return result;
}

}  // namespace

void auto_names(const std::wstring& path, bool kname_flg, bool vname_flg, const std::wstring& suffix,
                std::wstring* inst, std::wstring* out) {
    inst->clear();
    out->clear();
    size_t slash = path.find_last_of(L"\\/:");
    std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind(L'.');
    std::wstring base = dot == std::wstring::npos ? name : name.substr(0, dot);
    std::wstring pat = base;
    for (size_t i = 0; i < 2 && i < pat.size(); i++) {  // leading track number -> wildcard
        if (pat[i] < L'0' || pat[i] > L'9') break;
        pat[i] = L'?';
    }
    std::wstring sep, rest = suffix;
    if (!suffix.empty() && wcschr(L" _-([", suffix[0])) sep = suffix.substr(0, 1), rest = suffix.substr(1);
    rest += L".wav";
    long long ref = file_size(path);

    std::wstring out_name = base + sep + rest;
    std::wstring out_path = dir + out_name;
    std::wstring found = select(find_audio(dir, pat, ref), name, out_name, vname_flg);
    if (found.empty()) {
        std::wstring b = base, p = pat;
        for (size_t n = base.size(); n > 1; n--) {  // drop characters from the end
            b.pop_back();
            p.pop_back();
            std::wstring xname = (!sep.empty() && b.back() == sep[0]) ? b + rest : b + sep + rest;
            found = select(find_audio(dir, p, ref), name, xname, vname_flg);
            if (!found.empty()) {
                out_path = dir + xname;
                break;
            }
        }
    }
    if (kname_flg && !found.empty()) *inst = dir + found;
    if (vname_flg) *out = out_path;
}

std::wstring sanitize_suffix(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text)
        if (!wcschr(L"\\/:*?\"<>|", c)) out += c;
    return out;
}

}  // namespace utagoe
