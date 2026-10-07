#include "lang.hpp"

#include <windows.h>

namespace utagoe {

const Lang LANG_EN = {
    false, L"Tahoma", DEFAULT_CHARSET,
    L"Warning", L"Confirm", L"OK", L"Cancel", L"WAVE file (*.wav)", L"All files (*.*)",
    L"Input File (Original)", L"Input File (Instrumental)", L"Output .WAV File (Vocal)",
    L"Start", L"Abort", L"Quit", L"Settings...", L"Help...", L"Version Info",
    L"Please drop an audio file.",
    L"\"%ls\"\nThe file could not be opened.",
    L"\"%ls\"\nThis file format is not supported,\nor the file is damaged.",
    L"\"%ls\"\nThis WAVE file format is not supported.",
    L"%.3fkHz %d-bit %ls", L"Mono", L"Stereo",
    L"Abort processing?",
    L"Please specify the input and output file names.",
    L"The input file and the output file are the same.",
    L"The input files must have the same\nsampling rate and number of channels.",
    L"The sampling bit depth must be 16 bits.",
    L"\"%ls\"\nA file with the same name already exists. Overwrite it?",
    L"\"%ls\"\nThe file could not be created.",
    L"The help file could not be found.",
    L"Adobe Reader is required to display the help.",
    L"An error occurred while displaying the help.",
    L"Debug mode %ls",
    L"Memory could not be allocated.",
    L"An error occurred while writing the file.",
    L"HELP_EN", L"UtagoeHelp.html",
    L"Analyzing", L"Preparing", L"%ls  %d seconds elapsed",
    L"Settings", L"Processing Method", L"Advanced", L"Misc", L"Reset",
    {L"Intro Analysis", L"Automatic", L"Normal", L"Detailed", L"None"},
    {L"Extraction Method", L"By Frequency", L"By Waveform"},
    {L"Instrumental Level Adjustment", L"Automatic (Averaged)", L"Automatic (Adaptive)", L"Manual", L"None"},
    {L"Accuracy Priority", L"Quality", L"Extraction"},
    {L"Processing Mode", L"Normal", L"L/R Difference", L"Mono"},
    {L"Instrumental Phase", L"Automatic", L"Positive Phase", L"Inverted Phase"},
    L"Time Shift Correction", L"Automatic", L"Manual", L"Range", L"Extractable Level", L"Weak", L"Strong",
    L"Filtering", L"Extraction Centralization", L"Low Pass Filter", L"High Pass Filter", L"Oversampling",
    L"Multiplier:", L"(For \"By Waveform\" Method)", L"", L"Block Length", L"Millseconds",
    L"File Name Settings", L"Search For Instrumental File", L"Automatically Name Output File",
    L"Append To Filename:",
    {345, 43}, {427, 91}, 125, 110, 135, {32, 26}, {34, 48}, 8, 68, 105, 35, 16, 125,
    L"Playback", L"About 歌声りっぷ",
    // the en_US DFM has ＴＯＤＡＫＥＮ, which the ANSI program shows as TODAKEN on non-Japanese Windows
    L"(C)1999-2009 TODAKEN\n(C)2013 Partial en_US translation by DjLizard\nwww.DjLizard.net\n\n"
    L"Greets: Babylove, ToastyX, mrb,\nNicknerdface, iliketostayinside,\nall of tumblr #prfm fandom",
    240, 190, 3, 104, 240, 160, 208,
    L"Utagoe Rip 4", L"Improved extraction (recommended)",
    L"Matches EQ and level differences between the two releases, follows timing drift and speed "
    L"wobble, and detects inverted polarity. The settings it takes care of itself are greyed out.",
    L"Audio files", L"%.3fkHz %ls %ls", L"%d channels",
};

const Lang LANG_JA = {
    true, L"ＭＳ Ｐゴシック", SHIFTJIS_CHARSET,
    L"警告", L"確認", L"OK", L"キャンセル", L"WAVEファイル (*.wav)", L"すべてのファイル (*.*)",
    L"入力ファイル（オリジナル）", L"入力ファイル（カラオケ）", L"出力WAVEファイル（ボーカル）",
    L"作成開始", L"処理中止", L"終了", L"設定...      ", L"使い方...", L"バージョン情報",
    L"音声ファイルをドロップしてください。",
    L"\"%ls\"\nファイルが開けませんでした。",
    L"\"%ls\"\n扱えないファイル形式です。\nまたはファイルが破損しています。",
    L"\"%ls\"\n扱えない形式のWAVEファイルです。",
    L"%.3fkHz %dビット %ls", L"モノラル", L"ステレオ",
    L"処理を中止しますか。",
    L"入力ファイル名と出力ファイル名を指定してください。",
    L"入力ファイルと出力ファイルが同じです。",
    L"入力ファイルのサンプリングレート・チャンネル数は\n同じ条件にしてください。",
    L"サンプリングビット数は16ビットにしてください。",
    L"\"%ls\"\n同じ名前のファイルがあります。上書きしますか？",
    L"\"%ls\"\nファイルの作成に失敗しました。",
    L"ヘルプファイルが見つかりません。",
    L"ヘルプを表示するには Adobe Reader が必要です。",
    L"ヘルプ表示でエラーが発生しました。",
    L"デバッグモード %ls",
    L"メモリの確保に失敗しました。",
    L"ファイル作成中にエラーが発生しました。",
    L"HELP_JA", L"UtagoeHelp.pdf",
    L"自動解析中", L"初期処理中", L"%ls  %d秒経過",
    L"歌声りっぷ設定", L"  処理方法  ", L"  詳細設定  ", L"  補助機能  ", L"リセット",
    {L"イントロ解析", L"自動", L"ノーマル", L"詳細", L"なし"},
    {L"抽出方法", L"周波数合成", L"波形合成"},
    {L"カラオケ レベル調整", L"自動（平均）", L"自動（適応）", L"手動", L"なし"},
    {L"音質調整", L"音質優先", L"抽出優先"},
    {L"処理モード", L"ノーマル", L"左右差分", L"モノラル化"},
    {L"カラオケ位相", L"自動", L"正相", L"逆相"},
    L"時間ずれ補正", L"自動", L"手動", L"補正範囲", L"抽出レベル", L"弱", L"強",
    L"フィルター処理", L"中央定位抽出", L"高音カット", L"低音カット", L"オーバーサンプリング",
    L"有効", L"波形合成時に有効", L"倍", L"処理ブロックサイズ", L"ミリ秒",
    L"ファイル名設定機能", L"カラオケファイル自動検索", L"ボーカルファイル名自動設定", L"追加文字列",
    {356, 43}, {447, 91}, 97, 73, 119, {21, 32}, {84, 36}, 13, 78, 0, 0, 42, 112,
    L"WAVE再生", L"歌声りっぷについて", L"(C)1999-2009 ＴＯＤＡＫＥＮ ",
    159, 113, 104, 104, 0, 0, 128,
    L"歌声りっぷ4", L"改良版の抽出方式を使う（推奨）",
    L"２つの音源のEQ・音量の違いを補正し、時間のずれや回転ムラに追従、逆相も自動で判定します。"
    L"この方式が自動で行う設定は灰色になります。",
    L"音声ファイル", L"%.3fkHz %ls %ls", L"%dチャンネル",
};

const Lang& pick_language(const std::wstring& code) {
    if (!code.empty()) return (code[0] == L'j' || code[0] == L'J') ? LANG_JA : LANG_EN;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? LANG_JA : LANG_EN;
}

std::wstring log_line(const Lang& lang, const std::wstring& line) {
    if (!lang.japanese) return line;
    static const wchar_t* const map[][2] = {{L"Simple analysis        ", L"簡易解析     "},
                                            {L"Detailed analysis LR   ", L"詳細解析LR   "},
                                            {L"Detailed analysis mono ", L"詳細解析mono "}};
    for (const auto& m : map) {
        std::wstring en = m[0];
        if (line.compare(0, en.size(), en) == 0) return m[1] + line.substr(en.size());
    }
    const std::wstring io = L"Initial offset:";
    if (line.compare(0, io.size(), io) == 0) {
        size_t p = line.find(L" phase:");
        if (p != std::wstring::npos)
            return L"決定初期オフセット:" + line.substr(io.size(), p - io.size()) + L" 位相：" + line.substr(p + 7);
    }
    return line;
}

}  // namespace utagoe
