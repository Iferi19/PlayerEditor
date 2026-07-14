// PlayerEditor — C++ / Dear ImGui / GLFW / miniaudio / ffmpeg
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "misc/cpp/imgui_stdlib.h"
#include <GLFW/glfw3.h>

#include "portable-file-dialogs.h"

#include "audio_clip.h"
#include "player.h"
#include "wav_io.h"
#include "ffmpeg.h"
#include "effects.h"
#include "analysis.h"
#include "join.h"
#include "spectrum.h"
#include "stereo.h"
#include "reference.h"
#include "video_reader.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include "platform_utf8.h"
#endif

// バックグラウンド測定の受け渡し(スレッド寿命に依存しない共有状態)
struct MeasureState
{
    std::mutex m;
    bool ready = false;
    long long gen = 0;
    LoudnessResult result;
    bool ok = false;
};

// 曲全体平均スペクトラムの受け渡し(同上)
struct SpecAvgState
{
    std::mutex m;
    bool ready = false;
    long long gen = 0;
    std::vector<float> binsDb;
};

// 1タブ = 1ファイル分の状態
struct Doc
{
    AudioClip clip;
    std::string name;   // タブ表示名(ファイル名)

    long long playhead = 0;
    long long selStart = -1, selEnd = -1;

    std::vector<float> pkMin, pkMax;   // レーンごとに cols 個並ぶ (lane0, lane1)
    int pkWidth = -1;
    int pkLanes = 1;                   // 1=モノ表示, 2=ステレオL/R

    LoudnessResult loud;
    bool loudValid = false;
    std::string loudText;
    double playbackGainDb = 0.0;

    std::shared_ptr<MeasureState> ms = std::make_shared<MeasureState>();
    long long measureGen = 0;
    bool measuring = false;

    Tags tags;   // 書き出し時に埋め込む曲情報
    int srcBits = 0;          // ソースのビット深度(0=非PCM/不明)
    std::string fmtDetail;    // 表示用: "24 bit" / "MP3 320 kbps" 等

    // 解析パネル用キャッシュ(NaN=未計算。加工で無効化)
    double peakCache = NAN;
    double rmsCache = NAN;
    double corrAll = NAN;    // 曲全体の位相相関
    double widthAll = NAN;   // 曲全体のステレオ幅(%)
    std::vector<float> specDisp;   // スペクトラム表示の平滑化状態(ピクセル単位)

    // 曲全体の平均スペクトラム(バックグラウンド計算)
    std::shared_ptr<SpecAvgState> sas = std::make_shared<SpecAvgState>();
    long long specAvgGen = 0;
    bool specAvgComputing = false;
    std::vector<float> avgSpecBins;   // 計算結果(binごとのdB)。空=未計算

    // 映像(動画ファイルの場合)
    bool hasVideo = false;
    int srcW = 0, srcH = 0;      // 元解像度
    int vidW = 0, vidH = 0;      // デコード出力(縮小後)
    double vidFps = 0;
    VideoReader vreader;
    unsigned int tex = 0;        // GLテクスチャ(0=未作成)
    double lastPts = -1;         // 最後に表示したフレームのpts(-1=未表示)
    bool wantPoster = false;     // シーク直後: 次の1枚を即表示

    bool dragging = false;
    float downX = 0;
};

struct App
{
    std::vector<std::unique_ptr<Doc>> docs;
    int active = -1;
    Player player;
    bool normPlayback = true;   // 再生を -14 LUFS に（デフォルトON）
    bool loop = false;
    float volumePct = 100.0f;   // 音量スライダー(0-100%)
    float stereoWidthPct = 100.0f;   // ステレオ幅(0=モノ,100=原音,200=ワイド)
    int pendingSelect = -1;     // プログラム起因のタブ選択(切替時に停止させない)
    bool showAnalysis = false;  // 解析サイドパネル表示
    int refGenre = 0;           // 参考モデル(0=なし, 1..=RefModel::genres)
    int monitorIdx = 0;         // モニターシミュレーション(kMonitors)

    // 全画面(YouTube方式: ダブルクリック/Fで切替、Escで復帰)
    GLFWwindow* window = nullptr;
    bool fullscreen = false;
    int savedX = 0, savedY = 0, savedW = 0, savedH = 0;

    // 映像コントロールバー
    bool showVideoWave = false;   // 動画タブの波形(デフォルト非表示)
    double lastMouseMove = 0;     // 自動非表示用
    bool seekWasPlaying = false;  // シークバー操作前の再生状態
    bool videoTrimMode = false;   // フォト風トリム(シークバーにハンドル表示)
    bool mouseInWindow = true;    // カーソルがウィンドウ内にあるか(離脱で即バーを隠す)

    // 書き出し設定
    int exportFmt = 0;              // kFormats のインデックス
    bool exportUseSelection = false;
    bool exportNormalize = false;
    float exportTargetLufs = -14.0f;
    int exportSrIdx = 0;            // 0=元のまま
    int exportBitIdx = 0;           // 0=自動
    int exportPreset = 0;           // 0=カスタム
    bool exportVideoCopy = false;   // 動画: 映像ごと切り出し(再エンコードなし)
    bool wantExport = false;

    // 加工(エフェクト)設定
    bool wantFx = false;
    bool fxUseSelection = false;
    float fxGainDb = 0.0f;
    float fxFadeIn = 0.0f;    // 秒
    float fxFadeOut = 0.0f;   // 秒
    bool fxReverse = false;

    // タブ連結設定
    bool wantJoin = false;
    int joinOther = 0;          // 相手タブ(インデックス)
    float joinXfadeSec = 2.0f;  // クロスフェード秒
};

static Doc* curDoc(App& a)
{
    if (a.active >= 0 && a.active < (int)a.docs.size()) return a.docs[a.active].get();
    return nullptr;
}

static bool fileExists(const std::string& p) { std::ifstream f(p); return f.good(); }

static std::string baseName(const std::string& path)
{
    std::string b = path;
    auto slash = b.find_last_of("/\\");
    if (slash != std::string::npos) b = b.substr(slash + 1);
    return b;
}

static void loadFont(ImGuiIO& io)
{
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothR.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };
    for (const char* f : candidates)
        if (fileExists(f))
        {
            io.Fonts->AddFontFromFileTTF(f, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
            return;
        }
}

static void computePeaks(Doc& d, int cols)
{
    int ch = d.clip.channels;
    int lanes = (ch >= 2) ? 2 : 1;   // ステレオ以上は L/R の2段(3ch以上は先頭2ch)
    d.pkMin.assign((size_t)cols * lanes, 0.0f);
    d.pkMax.assign((size_t)cols * lanes, 0.0f);
    const auto& s = d.clip.samples;
    long long frames = d.clip.frameCount();

    for (int lane = 0; lane < lanes; lane++)
    {
        for (int c = 0; c < cols; c++)
        {
            long long f0 = (long long)((double)c / cols * frames);
            long long f1 = (long long)((double)(c + 1) / cols * frames);
            if (f1 <= f0) f1 = f0 + 1;
            float mn = 1.0f, mx = -1.0f;
            for (long long f = f0; f < f1 && f < frames; f++)
            {
                float v = s[(size_t)(f * ch + lane)];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (mn > mx) { mn = 0.0f; mx = 0.0f; }
            d.pkMin[(size_t)(lane * cols + c)] = mn;
            d.pkMax[(size_t)(lane * cols + c)] = mx;
        }
    }
    d.pkWidth = cols;
    d.pkLanes = lanes;
}

static std::pair<long long, long long> region(const Doc& d)
{
    if (d.selStart >= 0 && d.selEnd > d.selStart) return { d.selStart, d.selEnd };
    return { d.playhead, d.clip.frameCount() };
}

static std::string suggestName(const Doc& d, const char* suffix)
{
    std::string base = d.name.empty() ? "output" : d.name;
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base + suffix + ".wav";
}

static void showLoudness(Doc& d)
{
    double sp = d.clip.samplePeakDb();
    double gain = -14.0 - d.loud.integratedLufs;
    double predTp = d.loud.truePeakDb + gain;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "Integrated %.1f LUFS   |   True Peak %.1f dBTP   |   Sample Peak %.1f dBFS   ->   -14 LUFS まで Gain %+.1f dB%s",
        d.loud.integratedLufs, d.loud.truePeakDb, sp, gain,
        (predTp > 0 ? "   [!] 正規化後 True Peak が 0dBFS 超 -> クリップの恐れ" : ""));
    d.loudText = buf;
}

static void applyPlaybackGain(App& a)
{
    Doc* d = curDoc(a);
    float vol = a.volumePct / 100.0f;   // 音量スライダー(線形)
    if (a.normPlayback && d && d->loudValid)
    {
        d->playbackGainDb = -14.0 - d->loud.integratedLufs;
        a.player.setGain((float)std::pow(10.0, d->playbackGainDb / 20.0) * vol);
    }
    else
    {
        if (d) d->playbackGainDb = 0.0;
        a.player.setGain(vol);
    }
}

static std::string tempDir()
{
    const char* td = std::getenv("TEMP");
    if (!td) td = std::getenv("TMP");
#ifdef _WIN32
    return td ? td : ".";
#else
    return td ? td : "/tmp";
#endif
}

// 連結などで生まれたメモリ上のみのタブ(パス無し)は、一時WAVに書いてから測定する
static std::string ensureMeasurablePath(Doc& d)
{
    if (!d.clip.path.empty()) return d.clip.path;
    static int counter = 0;
    std::string p = tempDir() + "/pe_measure_" + std::to_string(++counter) + ".wav";
    std::string err;
    if (!WavIo::writeFloatWav(p, d.clip.samples, d.clip.channels, d.clip.sampleRate,
                              0, d.clip.frameCount(), 0.0, err))
        return "";
    return p;
}

static void startMeasure(Doc& d)
{
    if (d.measuring) return;
    if (!Ffmpeg::available()) { d.loudText = "ffmpeg が見つかりません（winget等で導入してください）。"; return; }

    std::string path = ensureMeasurablePath(d);
    if (path.empty()) { d.loudText = "測定用の一時ファイル作成に失敗しました。"; return; }
    bool isTemp = d.clip.path.empty();

    d.measuring = true;
    long long gen = ++d.measureGen;
    d.loudText = "測定中… (バックグラウンド)";

    auto ms = d.ms;
    std::thread([ms, path, gen, isTemp]()
    {
        std::string err;
        LoudnessResult r = Ffmpeg::measure(path, err);
        if (isTemp) std::remove(path.c_str());
        std::lock_guard<std::mutex> lk(ms->m);
        ms->result = r; ms->ok = r.ok; ms->gen = gen; ms->ready = true;
    }).detach();
}

static void pollMeasure(App& a)
{
    for (auto& up : a.docs)
    {
        Doc& d = *up;
        bool ready = false, ok = false; long long gen = 0; LoudnessResult r;
        {
            std::lock_guard<std::mutex> lk(d.ms->m);
            if (d.ms->ready) { ready = true; r = d.ms->result; ok = d.ms->ok; gen = d.ms->gen; d.ms->ready = false; }
        }
        if (!ready || gen != d.measureGen) continue;
        d.measuring = false;
        if (ok)
        {
            d.loud = r; d.loudValid = true; showLoudness(d);
            if (&d == curDoc(a)) applyPlaybackGain(a);
        }
        else d.loudText = "測定に失敗しました（ffmpeg）。";
    }
}

static bool ensureMeasuredSync(Doc& d)
{
    if (d.loudValid) return true;
    if (!Ffmpeg::available()) { d.loudText = "ffmpeg が見つかりません。"; return false; }
    std::string path = ensureMeasurablePath(d);
    if (path.empty()) { d.loudText = "測定用の一時ファイル作成に失敗しました。"; return false; }
    std::string err;
    auto r = Ffmpeg::measure(path, err);
    if (d.clip.path.empty()) std::remove(path.c_str());
    if (!r.ok) { d.loudText = "測定失敗: " + err; return false; }
    d.loud = r; d.loudValid = true; showLoudness(d);
    return true;
}

static const char* kFormats[] = { "WAV", "MP3", "M4A", "FLAC", "OGG", "AIF" };
static const char* kExts[]    = { "wav", "mp3", "m4a", "flac", "ogg", "aif" };

static const char* kSrItems[]  = { "元のまま", "44100 Hz", "48000 Hz", "96000 Hz" };
static const int   kSrValues[] = { 0, 44100, 48000, 96000 };
static const char* kBitItems[]  = { "ソースと同じ", "16 bit", "24 bit", "32 bit (float)" };
static const int   kBitValues[] = { -1, 16, 24, 32 };   // -1 = ソースのビット深度

// 媒体別プリセット(選択時に下のフィールドへ反映。以降は自由に編集可)
struct ExportPreset
{
    const char* name;
    int fmt;          // kFormats インデックス
    bool normalize;
    float target;     // LUFS
    int srIdx;        // kSrItems
    int bitIdx;       // kBitItems
};
// モニターシミュレーション(再生にのみ掛かる。書き出し・スペクトラムには影響しない)
struct MonitorProfile
{
    const char* name;
    bool mono;
    std::vector<Bq::Spec> specs;
};
static const std::vector<MonitorProfile> kMonitors = {
    { "フラット", false, {} },
    { "スマホ(最近の機種)", false, {
        // 現行フラッグシップ想定: ステレオ、~200Hzまで再生、中高域は比較的フラットで軽い輝き
        { Bq::Type::Highpass, 200, 0.707f, 0 },
        { Bq::Type::Highpass, 200, 0.707f, 0 },
        { Bq::Type::Peaking, 3000, 1.5f, 2 } } },
    { "スマホ(安価/旧機種)", true, {
        // 旧来のモノラル小口径: 低域なし+中域の張り
        { Bq::Type::Highpass, 500, 0.707f, 0 },
        { Bq::Type::Highpass, 500, 0.707f, 0 },
        { Bq::Type::Peaking, 2500, 2.0f, 4 },
        { Bq::Type::Lowpass, 15000, 0.707f, 0 } } },
    { "ノートPC", false, {
        { Bq::Type::Highpass, 250, 0.707f, 0 },
        { Bq::Type::Highpass, 250, 0.707f, 0 },
        { Bq::Type::Peaking, 1500, 1.5f, 3 } } },
    { "安いイヤホン", false, {
        { Bq::Type::Highpass, 120, 0.707f, 0 },
        { Bq::Type::Peaking, 300, 1.0f, -2 },
        { Bq::Type::Peaking, 8000, 1.5f, 4 } } },
    { "車内", false, {
        { Bq::Type::LowShelf, 100, 0.9f, 6 },
        { Bq::Type::Peaking, 1000, 1.0f, -2 },
        { Bq::Type::Lowpass, 15000, 0.707f, 0 } } },
    { "TV", false, {
        { Bq::Type::Highpass, 120, 0.707f, 0 },
        { Bq::Type::Peaking, 3000, 1.2f, 2 } } },
    { "電話(通話帯域)", true, {
        { Bq::Type::Highpass, 300, 0.707f, 0 },
        { Bq::Type::Highpass, 300, 0.707f, 0 },
        { Bq::Type::Lowpass, 3400, 0.707f, 0 },
        { Bq::Type::Lowpass, 3400, 0.707f, 0 } } },
};

static const ExportPreset kPresets[] = {
    { "カスタム(手動設定)",                      -1, false,   0.0f, -1, -1 },
    { "スマホ/ストリーミング (-14 LUFS, AAC)",     2, true,  -14.0f,  1,  0 },
    { "YouTube (-14 LUFS, 48kHz AAC)",             2, true,  -14.0f,  2,  0 },
    { "Apple Music (-16 LUFS, AAC)",               2, true,  -16.0f,  1,  0 },
    { "CDマスター (16bit/44.1kHz WAV)",            0, false,   0.0f,  1,  1 },
    { "放送 EBU R128 (-23 LUFS, 24bit/48k WAV)",   0, true,  -23.0f,  2,  2 },
    { "アーカイブ (FLAC 24bit, 元レート)",          3, false,   0.0f,  0,  2 },
};

// 既存ファイルなら上書き確認(ネイティブの重複確認は抑止し、日本語で1回だけ聞く)
static bool confirmOverwrite(const std::string& path)
{
    std::ifstream f(path);
    if (!f.good()) return true;
    auto r = pfd::message("上書きの確認",
        baseName(path) + " は既に存在します。上書きしますか？",
        pfd::choice::yes_no, pfd::icon::warning).result();
    return r == pfd::button::yes;
}

static std::string tempWavPath()
{
    const char* td = std::getenv("TEMP");
    if (!td) td = std::getenv("TMP");
#ifdef _WIN32
    std::string dir = td ? td : ".";
    return dir + "\\pe_export_tmp.wav";
#else
    std::string dir = td ? td : "/tmp";
    return dir + "/pe_export_tmp.wav";
#endif
}

static void doExport(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;

    long long s = 0, e = d->clip.frameCount();
    bool useSel = a.exportUseSelection && d->selStart >= 0 && d->selEnd > d->selStart;
    if (useSel) { s = d->selStart; e = d->selEnd; }

    // 映像ごと切り出し(再エンコードなし、キーフレーム精度)
    if (a.exportVideoCopy && d->hasVideo)
    {
        std::string srcExt = "mp4";
        auto sdot = d->clip.path.find_last_of('.');
        if (sdot != std::string::npos) srcExt = d->clip.path.substr(sdot + 1);

        std::string nm = d->name;
        auto ndot = nm.find_last_of('.');
        if (ndot != std::string::npos) nm = nm.substr(0, ndot);

        auto out = pfd::save_file("映像ごと切り出し", nm + "_cut." + srcExt,
            { "動画", "*." + srcExt }, pfd::opt::force_overwrite).result();
        if (out.empty() || !confirmOverwrite(out)) return;

        double t0 = s / (double)d->clip.sampleRate;
        double t1 = e / (double)d->clip.sampleRate;
        std::string err;
        if (Ffmpeg::cutVideoCopy(d->clip.path, out, t0, t1, err))
        {
            char buf[300];
            std::snprintf(buf, sizeof(buf), "映像ごと切り出し完了: %s (%.2f-%.2fs, 再エンコードなし)",
                          baseName(out).c_str(), t0, t1);
            d->loudText = buf;
        }
        else d->loudText = err;
        return;
    }

    double gain = 0.0;
    if (a.exportNormalize)
    {
        if (!ensureMeasuredSync(*d)) return;   // ffmpegで測定
        gain = (double)a.exportTargetLufs - d->loud.integratedLufs;
    }

    const char* ext = kExts[a.exportFmt];
    std::string nm = d->name;
    auto dot = nm.find_last_of('.');
    if (dot != std::string::npos) nm = nm.substr(0, dot);
    char sfx[32] = "";
    if (a.exportNormalize) std::snprintf(sfx, sizeof(sfx), "_%.0fLUFS", a.exportTargetLufs);
    std::string suggested = nm + sfx + (useSel ? "_trim" : "") + "." + ext;

    auto out = pfd::save_file("書き出し", suggested,
        { std::string(kFormats[a.exportFmt]) + " (*." + ext + ")", std::string("*.") + ext },
        pfd::opt::force_overwrite).result();
    if (out.empty() || !confirmOverwrite(out)) return;

    // 一時的に float WAV を作り、ffmpeg で目的形式へエンコード（タグ埋め込み）
    std::string tmp = tempWavPath();
    std::string err;
    if (!WavIo::writeFloatWav(tmp, d->clip.samples, d->clip.channels, d->clip.sampleRate, s, e, gain, err))
    { d->loudText = "書き出し失敗: " + err; return; }

    int bd = kBitValues[a.exportBitIdx];
    if (bd == -1) bd = (d->srcBits > 0 ? d->srcBits : 24);   // ソースと同じ(非PCMは24)
    if (bd != 16 && bd != 24 && bd != 32) bd = 24;
    bool ok = Ffmpeg::transcode(tmp, out, ext, d->tags, err, kSrValues[a.exportSrIdx], bd);
    std::remove(tmp.c_str());

    if (ok)
    {
        char norm[32] = "";
        if (a.exportNormalize) std::snprintf(norm, sizeof(norm), " / %.0f LUFS", a.exportTargetLufs);
        char buf[400];
        std::snprintf(buf, sizeof(buf), "書き出し完了: %s  [%s%s%s%s%s]",
            baseName(out).c_str(), kFormats[a.exportFmt], norm,
            useSel ? " / 選択範囲" : "",
            a.exportSrIdx > 0 ? (std::string(" / ") + kSrItems[a.exportSrIdx]).c_str() : "",
            (std::string(" / ") + std::to_string(bd) + "bit").c_str());
        d->loudText = buf;
    }
    else d->loudText = err;
}

static void playActive(App& a);   // 前方宣言(定義はタブ操作セクション)

// 現在のタブ + 相手タブ を等パワークロスフェードで連結し、新しいタブを作る
static void doJoin(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    if (a.joinOther < 0 || a.joinOther >= (int)a.docs.size() || a.joinOther == a.active)
    { d->loudText = "連結する相手タブを選んでください。"; return; }

    Doc& other = *a.docs[a.joinOther];
    if (other.clip.channels != d->clip.channels)
    { d->loudText = "チャンネル数が一致しません（現状は同一構成のみ連結可）。"; return; }
    if (other.clip.sampleRate != d->clip.sampleRate)
    { d->loudText = "サンプルレートが一致しません（現状は同一レートのみ連結可）。"; return; }

    long long xf = (long long)((double)a.joinXfadeSec * d->clip.sampleRate);
    auto joined = Join::crossfade(d->clip.samples, other.clip.samples, d->clip.channels, xf);

    a.player.stop();
    auto doc = std::make_unique<Doc>();
    doc->clip.samples = std::move(joined);
    doc->clip.channels = d->clip.channels;
    doc->clip.sampleRate = d->clip.sampleRate;
    doc->clip.path = "";   // メモリ上のみ(書き出しで保存)
    std::string an = d->name, bn = other.name;
    auto strip = [](std::string& s) { auto p = s.find_last_of('.'); if (p != std::string::npos) s = s.substr(0, p); };
    strip(an); strip(bn);
    doc->name = an + " + " + bn;
    doc->tags = d->tags;   // タグは先頭側を引き継ぐ

    a.docs.push_back(std::move(doc));
    a.active = (int)a.docs.size() - 1;
    a.pendingSelect = a.active;
    if (a.normPlayback) startMeasure(*a.docs.back());
    playActive(a);
}

static void drawJoinPopup(App& a)
{
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("join", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    Doc* d = curDoc(a);
    if (!d || a.docs.size() < 2) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    ImGui::Text("「%s」の後ろに連結する:", d->name.c_str());

    // 相手タブの選択(自分以外)
    if (a.joinOther == a.active) a.joinOther = (a.active + 1) % (int)a.docs.size();
    std::string preview = a.docs[a.joinOther]->name;
    if (ImGui::BeginCombo("相手タブ", preview.c_str()))
    {
        for (int i = 0; i < (int)a.docs.size(); i++)
        {
            if (i == a.active) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(a.docs[i]->name.c_str(), i == a.joinOther)) a.joinOther = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::InputFloat("クロスフェード (秒)", &a.joinXfadeSec, 0.5f, 1.0f, "%.1f");
    if (a.joinXfadeSec < 0) a.joinXfadeSec = 0;
    ImGui::TextDisabled("※ 等パワークロスフェード。結果は新しいタブになります(書き出しで保存)。");

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("連結", ImVec2(120, 0))) { doJoin(a); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// 解析パネル/保存で使うキャッシュを確保(全走査系は一度だけ計算)
static void ensureAnalysisCache(Doc& d)
{
    if (std::isnan(d.peakCache)) d.peakCache = d.clip.samplePeakDb();
    if (std::isnan(d.rmsCache)) d.rmsCache = d.clip.rmsDb();
    if (d.clip.channels >= 2 && std::isnan(d.corrAll))
    {
        auto sm = Stereo::measure(d.clip.samples, d.clip.channels, 0, d.clip.frameCount());
        d.corrAll = sm.correlation;
        d.widthAll = sm.widthPct;
    }
}

static Analysis::Data buildAnalysis(Doc& d)
{
    ensureAnalysisCache(d);
    Analysis::Data ad;
    ad.file = d.name;
    ad.durationSec = d.clip.duration();
    ad.sampleRate = d.clip.sampleRate;
    ad.channels = d.clip.channels;
    ad.frames = d.clip.frameCount();
    ad.samplePeakDbfs = d.peakCache;
    ad.rmsDbfs = d.rmsCache;
    ad.phaseCorrelation = d.corrAll;
    ad.stereoWidthPct = d.widthAll;
    if (d.loudValid)
    {
        ad.integratedLufs = d.loud.integratedLufs;
        ad.truePeakDbtp = d.loud.truePeakDb;
        ad.lraLu = d.loud.loudnessRange;
    }
    return ad;
}

// 解析レポートを保存(.json / .txt)。ラウドネスは可能なら測定して含める。
static void doAnalysis(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;

    if (!d->loudValid && Ffmpeg::available()) ensureMeasuredSync(*d);
    Analysis::Data ad = buildAnalysis(*d);

    std::string nm = d->name;
    auto dot = nm.find_last_of('.');
    if (dot != std::string::npos) nm = nm.substr(0, dot);
    auto out = pfd::save_file("解析結果を保存", nm + "_analysis.json",
        { "JSON", "*.json", "テキスト", "*.txt" }, pfd::opt::force_overwrite).result();
    if (out.empty() || !confirmOverwrite(out)) return;

    bool asText = out.size() > 4 && out.substr(out.size() - 4) == ".txt";
    std::ofstream f(out, std::ios::binary);
    if (!f) { d->loudText = "解析結果の保存に失敗しました。"; return; }
    f << (asText ? Analysis::toText(ad) : Analysis::toJson(ad));
    d->loudText = "解析結果を保存しました: " + baseName(out);
}

// 曲全体の平均スペクトラムをバックグラウンドで計算(サンプルはコピーして渡す)
static void startAvgSpectrum(Doc& d)
{
    if (d.specAvgComputing || !d.avgSpecBins.empty()) return;
    d.specAvgComputing = true;
    long long gen = ++d.specAvgGen;

    auto sas = d.sas;
    std::vector<float> samplesCopy = d.clip.samples;   // タブが閉じられても安全なようコピー
    int ch = d.clip.channels;
    long long frames = d.clip.frameCount();
    std::thread([sas, gen, ch, frames, samples = std::move(samplesCopy)]()
    {
        // 平均は時間分解能が不要なので大きいFFTで低域の実解像度を稼ぐ(~2.7Hz)
        auto bins = Spec::averageSpectrumDb(samples, ch, frames, 16384);
        std::lock_guard<std::mutex> lk(sas->m);
        sas->binsDb = std::move(bins);
        sas->gen = gen;
        sas->ready = true;
    }).detach();
}

static void pollAvgSpectrum(Doc& d)
{
    std::vector<float> bins;
    long long gen = 0;
    {
        std::lock_guard<std::mutex> lk(d.sas->m);
        if (!d.sas->ready) return;
        bins = std::move(d.sas->binsDb);
        gen = d.sas->gen;
        d.sas->ready = false;
    }
    if (gen != d.specAvgGen) return;   // 古い結果は破棄
    d.avgSpecBins = std::move(bins);
    d.specAvgComputing = false;
}

// 解析サイドパネル(右側)。値はライブ表示、保存はここから。
static void drawAnalysisPanel(App& a, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("analysisPanel", size, ImGuiChildFlags_None);

    Doc* d = curDoc(a);
    if (d)
    {
        ensureAnalysisCache(*d);
        // 未測定なら自動でバックグラウンド測定を開始
        if (!d->loudValid && !d->measuring && Ffmpeg::available()) startMeasure(*d);

        ImGui::SetCursorPos(ImVec2(10, 8));
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(0.62f, 0.82f, 0.98f, 1.0f), "解析");

        // 参考モデル(ジャンル別の目安)。デフォルトなし、控えめに右側へ。
        ImGui::SameLine(size.x - 20 - 150);
        ImGui::SetNextItemWidth(150);
        const auto& refs = RefModel::genres();
        const char* refCur = a.refGenre == 0 ? "参考: なし"
                                             : refs[(size_t)(a.refGenre - 1)].name;
        if (ImGui::BeginCombo("##refg", refCur))
        {
            if (ImGui::Selectable("なし", a.refGenre == 0)) a.refGenre = 0;
            for (int i = 0; i < (int)refs.size(); i++)
                if (ImGui::Selectable(refs[(size_t)i].name, a.refGenre == i + 1)) a.refGenre = i + 1;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ジャンル別のモデルケース(目安)をメーターとスペクトラムに重ねます。\n一般的な傾向のモデルであり、実測統計ではありません。");
        const RefModel::Genre* ref = a.refGenre > 0 ? &refs[(size_t)(a.refGenre - 1)] : nullptr;

        ImGui::Separator();

        auto row = [](const char* label, const std::string& value) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(130);
            ImGui::TextUnformatted(value.c_str());
        };
        char b[64];

        std::snprintf(b, sizeof(b), "%.2f s", d->clip.duration());
        row("長さ", b);
        std::snprintf(b, sizeof(b), "%d Hz / %dch", d->clip.sampleRate, d->clip.channels);
        row("フォーマット", b);
        row("ビット/形式", d->fmtDetail.empty() ? "-" : d->fmtDetail);

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("ラウドネス (EBU R128)");
        ImGui::Separator();
        if (d->loudValid)
        {
            std::snprintf(b, sizeof(b), "%.1f LUFS", d->loud.integratedLufs);
            row("Integrated", b);
            std::snprintf(b, sizeof(b), "%.1f dBTP", d->loud.truePeakDb);
            row("True Peak", b);
            std::snprintf(b, sizeof(b), "%.1f LU", d->loud.loudnessRange);
            row("LRA", b);
            std::snprintf(b, sizeof(b), "%+.1f dB", -14.0 - d->loud.integratedLufs);
            row("-14までのGain", b);
        }
        else if (d->measuring) row("Integrated", "測定中…");
        else row("Integrated", Ffmpeg::available() ? "-" : "(ffmpeg無し)");

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("レベル");
        ImGui::Separator();
        std::snprintf(b, sizeof(b), "%.1f dBFS", d->peakCache);
        row("Sample Peak", b);
        std::snprintf(b, sizeof(b), "%.1f dBFS", d->rmsCache);
        row("RMS", b);

        // ---- ステレオ(位相相関 / 幅 / ゴニオメーター) ----
        if (d->clip.channels >= 2)
        {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("ステレオ (再生位置 / 全体)");
            ImGui::Separator();

            const long long win = 8192;
            auto rt = Stereo::measure(d->clip.samples, d->clip.channels,
                                      d->playhead - win / 2, d->playhead + win / 2);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float bw = size.x - 20.0f;

            // 位相相関メーター (-1 .. +1)
            std::snprintf(b, sizeof(b), "%+.2f  (全体 %+.2f)", rt.correlation, d->corrAll);
            row("位相相関", b);
            {
                ImVec2 q0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(bw, 12));
                dl->AddRectFilled(q0, ImVec2(q0.x + bw, q0.y + 8), IM_COL32(40, 40, 44, 255), 2.0f);
                if (ref)   // 参考モデルの推奨レンジを薄い帯で
                {
                    float rx0 = q0.x + (ref->corrMin + 1.0f) * 0.5f * bw;
                    float rx1 = q0.x + (ref->corrMax + 1.0f) * 0.5f * bw;
                    dl->AddRectFilled(ImVec2(rx0, q0.y), ImVec2(rx1, q0.y + 8), IM_COL32(255, 255, 255, 34), 2.0f);
                }
                float cxm = q0.x + bw * 0.5f;
                dl->AddLine(ImVec2(cxm, q0.y), ImVec2(cxm, q0.y + 8), IM_COL32(90, 90, 95, 255));
                dl->AddText(ImVec2(q0.x - 2, q0.y + 9), IM_COL32(110, 110, 115, 255), "-1");
                dl->AddText(ImVec2(q0.x + bw - 14, q0.y + 9), IM_COL32(110, 110, 115, 255), "+1");
                // マーカー: 正=ティール / 0付近=黄 / 負=赤(位相問題)
                ImU32 mc = rt.correlation >= 0.2 ? IM_COL32(78, 201, 176, 255)
                         : rt.correlation >= -0.2 ? IM_COL32(255, 208, 64, 255)
                                                  : IM_COL32(235, 90, 90, 255);
                float mx = q0.x + (float)((rt.correlation + 1.0) * 0.5) * bw;
                dl->AddRectFilled(ImVec2(mx - 3, q0.y - 2), ImVec2(mx + 3, q0.y + 10), mc, 2.0f);
                // 全体値の細マーカー
                float ax = q0.x + (float)((d->corrAll + 1.0) * 0.5) * bw;
                dl->AddLine(ImVec2(ax, q0.y - 2), ImVec2(ax, q0.y + 10), IM_COL32(242, 166, 76, 220), 2.0f);
                ImGui::Dummy(ImVec2(0, 12));
            }

            // ステレオ幅 (0 .. 200%)
            std::snprintf(b, sizeof(b), "%.0f%%  (全体 %.0f%%)", rt.widthPct, d->widthAll);
            row("ステレオ幅", b);
            {
                ImVec2 q0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(bw, 10));
                dl->AddRectFilled(q0, ImVec2(q0.x + bw, q0.y + 8), IM_COL32(40, 40, 44, 255), 2.0f);
                if (ref)   // 参考モデルの推奨レンジ
                {
                    float rx0 = q0.x + (ref->widthMin / 200.0f) * bw;
                    float rx1 = q0.x + (ref->widthMax / 200.0f) * bw;
                    dl->AddRectFilled(ImVec2(rx0, q0.y), ImVec2(rx1, q0.y + 8), IM_COL32(255, 255, 255, 34), 2.0f);
                }
                float fw = (float)std::clamp(rt.widthPct / 200.0, 0.0, 1.0) * bw;
                dl->AddRectFilled(q0, ImVec2(q0.x + fw, q0.y + 8), IM_COL32(78, 201, 176, 230), 2.0f);
                float ax = q0.x + (float)std::clamp(d->widthAll / 200.0, 0.0, 1.0) * bw;
                dl->AddLine(ImVec2(ax, q0.y - 2), ImVec2(ax, q0.y + 10), IM_COL32(242, 166, 76, 220), 2.0f);
                ImGui::Dummy(ImVec2(0, 4));
            }

            // ゴニオメーター (リサージュ、45度回転: 縦=Mid 横=Side)
            {
                float gs = std::min(bw, 140.0f);
                ImVec2 g0 = ImGui::GetCursorScreenPos();
                g0.x += (bw - gs) * 0.5f;   // 中央寄せ
                ImGui::Dummy(ImVec2(bw, gs + 4));
                ImVec2 g1(g0.x + gs, g0.y + gs);
                dl->AddRectFilled(g0, g1, IM_COL32(14, 14, 16, 255), 3.0f);
                // ガイド: 縦(M)/横(S)軸と±45度(L/R軸)
                ImVec2 gc(g0.x + gs * 0.5f, g0.y + gs * 0.5f);
                dl->AddLine(ImVec2(gc.x, g0.y), ImVec2(gc.x, g1.y), IM_COL32(45, 45, 50, 255));
                dl->AddLine(ImVec2(g0.x, gc.y), ImVec2(g1.x, gc.y), IM_COL32(45, 45, 50, 255));
                dl->AddLine(g0, g1, IM_COL32(38, 38, 42, 255));
                dl->AddLine(ImVec2(g0.x, g1.y), ImVec2(g1.x, g0.y), IM_COL32(38, 38, 42, 255));
                dl->AddText(ImVec2(g0.x + 3, g0.y + 2), IM_COL32(110, 110, 115, 200), "L");
                dl->AddText(ImVec2(g1.x - 12, g0.y + 2), IM_COL32(110, 110, 115, 200), "R");

                // 点群: 再生位置周辺のサンプルを 45度回転 (x=S, y=M) でプロット
                const auto& smp = d->clip.samples;
                int ch = d->clip.channels;
                long long total = d->clip.frameCount();
                long long f0 = std::max(0LL, d->playhead - win / 2);
                long long f1 = std::min(total, d->playhead + win / 2);
                long long step = std::max(1LL, (f1 - f0) / 1200);
                float r = gs * 0.48f;
                for (long long f = f0; f < f1; f += step)
                {
                    float L = smp[(size_t)(f * ch)];
                    float R = smp[(size_t)(f * ch + 1)];
                    float x = (L - R) * 0.7071f;
                    float y = (L + R) * 0.7071f;
                    float px = gc.x + std::clamp(x, -1.0f, 1.0f) * r;
                    float py = gc.y - std::clamp(y, -1.0f, 1.0f) * r;
                    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + 1.5f, py + 1.5f),
                                      IM_COL32(78, 201, 176, 120));
                }
            }
        }

        // ---- スペクトラム(連続曲線: 現在位置 + 曲全体平均) ----
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("スペクトラム");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.31f, 0.79f, 0.69f, 1.0f), "■現在");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), "―全体平均");
        if (ref)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.87f, 0.9f), "--%s(目安)", ref->name);
        }
        ImGui::Separator();
        {
            const int   FFTN = 4096;
            const float FLOOR = -66.0f;  // 表示下限(dB)

            float sw = size.x - 20.0f, sh = 150.0f;
            ImVec2 sp0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("spec", ImVec2(sw, sh));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 sp1(sp0.x + sw, sp0.y + sh);
            dl->AddRectFilled(sp0, sp1, IM_COL32(14, 14, 16, 255));

            int nPts = std::max(32, (int)sw);   // 1px = 1点の連続曲線

            // 全体平均: 必要なら計算開始、完了していれば取り込み
            startAvgSpectrum(*d);
            pollAvgSpectrum(*d);

            // 現在位置のスペクトラム(ピクセル解像度で対数リサンプル)
            auto mags = Spec::magnitudesDb(d->clip.samples, d->clip.channels,
                                           d->clip.frameCount(), d->playhead, FFTN);
            auto live = Spec::bandLevelsDb(mags, d->clip.sampleRate, FFTN, nPts, 20.0f, 20000.0f);

            if ((int)d->specDisp.size() != nPts) d->specDisp.assign((size_t)nPts, FLOOR);
            for (int i = 0; i < nPts; i++)
            {
                float t = std::max(live[(size_t)i], FLOOR);
                float& disp = d->specDisp[(size_t)i];
                disp += (t - disp) * (t > disp ? 0.5f : 0.12f);   // アタック速め/リリース遅め
            }

            // dBグリッド(-20/-40)
            for (float g : { -20.0f, -40.0f })
            {
                float gy = sp0.y + (g / FLOOR) * sh;
                dl->AddLine(ImVec2(sp0.x, gy), ImVec2(sp1.x, gy), IM_COL32(50, 50, 55, 255));
                char gb[8];
                std::snprintf(gb, sizeof(gb), "%.0f", g);
                dl->AddText(ImVec2(sp0.x + 2, gy - 14), IM_COL32(110, 110, 115, 255), gb);
            }

            // 現在位置: 連続曲線(下を塗りつぶし)
            for (int i = 0; i < nPts; i++)
            {
                float norm = std::clamp((d->specDisp[(size_t)i] - FLOOR) / -FLOOR, 0.0f, 1.0f);
                float x = sp0.x + (float)i * sw / nPts;
                float y = sp1.y - norm * sh;
                dl->AddLine(ImVec2(x, y), ImVec2(x, sp1.y), IM_COL32(78, 201, 176, 110));
            }
            // 輪郭線
            {
                std::vector<ImVec2> pts((size_t)nPts);
                for (int i = 0; i < nPts; i++)
                {
                    float norm = std::clamp((d->specDisp[(size_t)i] - FLOOR) / -FLOOR, 0.0f, 1.0f);
                    pts[(size_t)i] = ImVec2(sp0.x + (float)i * sw / nPts, sp1.y - norm * sh);
                }
                dl->AddPolyline(pts.data(), nPts, IM_COL32(78, 201, 176, 255), 0, 1.5f);
            }

            // 曲全体平均: オレンジのライン
            if (!d->avgSpecBins.empty())
            {
                int avgFftN = (int)d->avgSpecBins.size() * 2;   // 計算時のFFTサイズ
                auto avg = Spec::bandLevelsDb(d->avgSpecBins, d->clip.sampleRate, avgFftN, nPts, 20.0f, 20000.0f);
                std::vector<ImVec2> pts((size_t)nPts);
                for (int i = 0; i < nPts; i++)
                {
                    float norm = std::clamp((avg[(size_t)i] - FLOOR) / -FLOOR, 0.0f, 1.0f);
                    pts[(size_t)i] = ImVec2(sp0.x + (float)i * sw / nPts, sp1.y - norm * sh);
                }
                dl->AddPolyline(pts.data(), nPts, IM_COL32(242, 166, 76, 255), 0, 1.5f);

                // 参考モデル: トラック自身の1kHzレベルに合わせて配置した破線(目安)
                if (ref)
                {
                    int i1k = (int)(nPts * std::log(1000.0f / 20.0f) / std::log(1000.0f));
                    i1k = std::clamp(i1k, 0, nPts - 1);
                    float offset = avg[(size_t)i1k];   // モデルの0dB(=1kHz)をここへ
                    for (int i = 0; i + 3 < nPts; i += 6)   // 破線: 3px描いて3px空ける
                    {
                        auto py = [&](int idx) {
                            float hz = 20.0f * std::pow(1000.0f, (float)idx / nPts);
                            float dbv = offset + RefModel::specAt(*ref, hz);
                            float nm = std::clamp((dbv - FLOOR) / -FLOOR, 0.0f, 1.0f);
                            return sp1.y - nm * sh;
                        };
                        dl->AddLine(ImVec2(sp0.x + (float)i * sw / nPts, py(i)),
                                    ImVec2(sp0.x + (float)(i + 3) * sw / nPts, py(i + 3)),
                                    IM_COL32(220, 220, 225, 150), 1.0f);
                    }
                }
            }
            else if (d->specAvgComputing)
            {
                dl->AddText(ImVec2(sp0.x + sw * 0.5f - 45, sp0.y + 4),
                            IM_COL32(242, 166, 76, 200), "全体平均: 計算中…");
            }

            // 周波数目盛 (100 / 1k / 10k)
            struct { float hz; const char* lb; } marks[] = { {100, "100"}, {1000, "1k"}, {10000, "10k"} };
            for (auto& mk : marks)
            {
                float t = std::log(mk.hz / 20.0f) / std::log(20000.0f / 20.0f);
                float mx = sp0.x + t * sw;
                dl->AddLine(ImVec2(mx, sp1.y - 5), ImVec2(mx, sp1.y), IM_COL32(140, 140, 145, 255));
                dl->AddText(ImVec2(mx - 8, sp1.y - 18), IM_COL32(140, 140, 145, 255), mk.lb);
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        if (ImGui::Button("ファイルに保存…", ImVec2(size.x - 20, 0))) doAnalysis(a);
        ImGui::EndGroup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void applyFx(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;

    long long s = 0, e = d->clip.frameCount();
    bool useSel = a.fxUseSelection && d->selStart >= 0 && d->selEnd > d->selStart;
    if (useSel) { s = d->selStart; e = d->selEnd; }

    a.player.stop();
    int ch = d->clip.channels;
    int sr = d->clip.sampleRate;

    if (a.fxGainDb != 0.0f) Fx::gainDb(d->clip.samples, ch, s, e, a.fxGainDb);
    if (a.fxFadeIn > 0.0f) Fx::fadeIn(d->clip.samples, ch, s, e, (long long)(a.fxFadeIn * sr));
    if (a.fxFadeOut > 0.0f) Fx::fadeOut(d->clip.samples, ch, s, e, (long long)(a.fxFadeOut * sr));
    if (a.fxReverse) Fx::reverse(d->clip.samples, ch, s, e);

    d->pkWidth = -1;        // 波形を再計算
    d->loudValid = false;   // ラウドネスは変わったので測り直し
    d->peakCache = d->rmsCache = NAN;   // 解析パネルのキャッシュも無効化
    d->corrAll = d->widthAll = NAN;
    d->avgSpecBins.clear();             // 平均スペクトラムも無効化(進行中の結果は世代で破棄)
    d->specAvgGen++;
    d->specAvgComputing = false;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "加工を適用しました (%s%s%s%s%s) ※メモリ上のみ、書き出しで保存",
        useSel ? "選択範囲" : "全体",
        a.fxGainDb != 0.0f ? " / ゲイン" : "",
        a.fxFadeIn > 0.0f ? " / フェードイン" : "",
        a.fxFadeOut > 0.0f ? " / フェードアウト" : "",
        a.fxReverse ? " / リバース" : "");
    d->loudText = buf;
}

static void drawFxPopup(App& a)
{
    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("fx", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    Doc* d = curDoc(a);
    if (!d) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    bool hasSel = (d->selStart >= 0 && d->selEnd > d->selStart);
    ImGui::TextUnformatted("対象:");
    ImGui::SameLine();
    if (ImGui::RadioButton("全体", !a.fxUseSelection)) a.fxUseSelection = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::RadioButton("選択範囲", a.fxUseSelection)) a.fxUseSelection = true;
    ImGui::EndDisabled();
    if (!hasSel) a.fxUseSelection = false;

    ImGui::SeparatorText("エフェクト");
    ImGui::SliderFloat("ゲイン (dB)", &a.fxGainDb, -24.0f, 24.0f, "%.1f dB");
    ImGui::InputFloat("フェードイン (秒)", &a.fxFadeIn, 0.1f, 1.0f, "%.2f");
    ImGui::InputFloat("フェードアウト (秒)", &a.fxFadeOut, 0.1f, 1.0f, "%.2f");
    if (a.fxFadeIn < 0) a.fxFadeIn = 0;
    if (a.fxFadeOut < 0) a.fxFadeOut = 0;
    ImGui::Checkbox("リバース(逆再生化)", &a.fxReverse);
    ImGui::TextDisabled("※ メモリ上のデータに適用します（元ファイルは変更されません）。");

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("適用", ImVec2(120, 0))) { applyFx(a); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void drawExportPopup(App& a)
{
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("export", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    Doc* d = curDoc(a);
    if (!d) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    bool hasSel = (d->selStart >= 0 && d->selEnd > d->selStart);

    // ---- 媒体別プリセット ----
    {
        auto names = [](int i) { return kPresets[i].name; };
        if (ImGui::BeginCombo("プリセット", kPresets[a.exportPreset].name))
        {
            for (int i = 0; i < IM_ARRAYSIZE(kPresets); i++)
            {
                if (ImGui::Selectable(names(i), i == a.exportPreset))
                {
                    a.exportPreset = i;
                    const auto& p = kPresets[i];
                    if (p.fmt >= 0)   // カスタム以外は設定を反映
                    {
                        a.exportFmt = p.fmt;
                        a.exportNormalize = p.normalize;
                        if (p.normalize) a.exportTargetLufs = p.target;
                        a.exportSrIdx = p.srIdx;
                        a.exportBitIdx = p.bitIdx;
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    ImGui::TextUnformatted("範囲:");
    ImGui::SameLine();
    if (ImGui::RadioButton("全体", !a.exportUseSelection)) a.exportUseSelection = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::RadioButton("選択範囲", a.exportUseSelection)) a.exportUseSelection = true;
    ImGui::EndDisabled();
    if (!hasSel) a.exportUseSelection = false;

    if (d->hasVideo)
    {
        ImGui::Checkbox("映像ごと切り出し(再エンコードなし)", &a.exportVideoCopy);
        if (a.exportVideoCopy)
            ImGui::TextDisabled("※ ストリームコピー。開始点はキーフレーム精度、正規化・形式変換は適用されません。");
    }
    else a.exportVideoCopy = false;

    ImGui::BeginDisabled(a.exportVideoCopy);
    ImGui::Checkbox("ラウドネス正規化", &a.exportNormalize);
    ImGui::SameLine();
    ImGui::BeginDisabled(!a.exportNormalize);
    ImGui::SetNextItemWidth(110);
    ImGui::InputFloat("目標 LUFS", &a.exportTargetLufs, 1.0f, 2.0f, "%.1f");
    if (a.exportTargetLufs > 0) a.exportTargetLufs = 0;
    ImGui::EndDisabled();

    ImGui::Combo("形式", &a.exportFmt, kFormats, IM_ARRAYSIZE(kFormats));
    ImGui::Combo("サンプルレート", &a.exportSrIdx, kSrItems, IM_ARRAYSIZE(kSrItems));
    bool bitApplies = (a.exportFmt == 0 || a.exportFmt == 3 || a.exportFmt == 5);   // WAV/FLAC/AIF
    ImGui::BeginDisabled(!bitApplies);
    ImGui::Combo("ビット深度", &a.exportBitIdx, kBitItems, IM_ARRAYSIZE(kBitItems));
    if (bitApplies && a.exportBitIdx == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("読み込んだ曲のビット深度（%s）で書き出します。",
                          d->srcBits > 0 ? (std::to_string(d->srcBits) + "bit").c_str() : "非PCM→24bit");
    ImGui::EndDisabled();
    ImGui::EndDisabled();   // exportVideoCopy

    ImGui::SeparatorText("曲情報（タグ）");
    ImGui::InputText("タイトル", &d->tags.title);
    ImGui::InputText("アーティスト", &d->tags.artist);
    ImGui::InputText("アルバムタイトル", &d->tags.album);
    ImGui::InputText("アルバムアーティスト", &d->tags.albumArtist);
    ImGui::InputText("ジャンル", &d->tags.genre);
    ImGui::InputText("年", &d->tags.year);
    ImGui::InputText("トラック番号", &d->tags.track);
    ImGui::TextDisabled("※ 全形式に埋め込みます（AIF/WAV含む）。");

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("書き出す", ImVec2(130, 0))) { doExport(a); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル", ImVec2(130, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --- タブ操作 / 再生 ---

static void playActive(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    applyPlaybackGain(a);
    a.player.setLoop(a.loop);
    a.player.play(d->clip, 0, d->clip.frameCount());
    d->playhead = 0;
}

// アクティブタブを現在位置(または選択範囲)から再生開始
static void startPlayback(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    applyPlaybackGain(a);
    a.player.setLoop(a.loop);
    bool hasSel = (d->selStart >= 0 && d->selEnd > d->selStart);
    // 最後まで再生済みなら先頭へ戻して再生(終端から始まって無音になるのを防ぐ)
    if (!hasSel && d->playhead >= d->clip.frameCount() - 1)
        d->playhead = 0;
    auto [s, e] = region(*d);
    // ループ戻り先: 選択があれば選択先頭、なければ曲頭(途中から再生してもループは最初へ)
    a.player.play(d->clip, s, e, hasSel ? s : 0);
}

static void togglePlay(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    if (a.player.isPlaying()) a.player.stop();
    else if (a.player.isPaused()) a.player.resume();
    else startPlayback(a);
}

// 再生位置を曲頭(0)へ。再生中なら先頭から再生し直す。
static void goToStart(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    bool wasPlaying = a.player.isPlaying();
    a.player.stop();
    d->playhead = 0;
    if (wasPlaying)
    {
        applyPlaybackGain(a);
        a.player.setLoop(a.loop);
        a.player.play(d->clip, 0, d->clip.frameCount(), 0);
    }
}

static bool addDocNoPlay(App& a, const std::string& path)
{
    AudioClip c;
    std::string err;
    if (!AudioClip::load(path, c, err))
    {
        if (Doc* d = curDoc(a)) d->loudText = "読み込み失敗: " + err;
        return false;
    }
    auto doc = std::make_unique<Doc>();
    doc->clip = std::move(c);
    doc->name = baseName(path);
    doc->tags = Ffmpeg::readTags(path);   // 入力に埋まっている曲情報を流用(無ければ空)

    // ストリーム情報(ビット深度/コーデック/映像)を取得
    if (Ffmpeg::available())
    {
        auto si = Ffmpeg::probe(path);

        // ビット深度・形式の表示文字列
        doc->srcBits = si.audioBits;
        if (si.audioBits > 0)
        {
            char fb[48];
            std::snprintf(fb, sizeof(fb), "%d bit", si.audioBits);
            doc->fmtDetail = fb;
            if (si.codecName == "flac") doc->fmtDetail += " FLAC";
        }
        else if (!si.codecName.empty())
        {
            std::string up = si.codecName;
            for (char& ch2 : up) ch2 = (char)toupper((unsigned char)ch2);
            doc->fmtDetail = up;
            if (si.bitRateKbps > 0)
                doc->fmtDetail += " " + std::to_string(si.bitRateKbps) + " kbps";
        }

        if (si.hasVideo)
        {
            doc->hasVideo = true;
            doc->srcW = si.width;
            doc->srcH = si.height;
            doc->vidFps = si.fps;
            // デコード上限 = モニター解像度(全画面で等倍になるように)。範囲[1280, 3840]。
            int cap = 1920;
            if (GLFWmonitor* mon = glfwGetPrimaryMonitor())
                if (const GLFWvidmode* mode = glfwGetVideoMode(mon))
                    cap = std::clamp(mode->width, 1280, 3840);
            int outW = std::min(cap, si.width);
            int outH = (int)((long long)si.height * outW / si.width);
            if (outH < 2) outH = 2;
            doc->vidW = outW - (outW % 2);
            doc->vidH = outH - (outH % 2);
        }
    }

    a.docs.push_back(std::move(doc));
    a.active = (int)a.docs.size() - 1;
    a.pendingSelect = a.active;           // プログラム起因の選択(タブ切替停止を抑止)
    if (a.normPlayback) startMeasure(*a.docs.back());  // -14 用の測定はバックグラウンド
    return true;
}

// 複数ファイル → それぞれ別タブ。最後のタブをアクティブにして自動再生。
static void openFiles(App& a, const std::vector<std::string>& paths)
{
    a.player.stop();   // 排他: 追加中は止める
    bool any = false;
    for (const auto& p : paths) if (addDocNoPlay(a, p)) any = true;
    if (any) playActive(a);
}

static void closeDoc(App& a, int i)
{
    if (i < 0 || i >= (int)a.docs.size()) return;
    a.player.stop();
    if (a.docs[i]->tex)
    {
        GLuint t = a.docs[i]->tex;
        glDeleteTextures(1, &t);
    }
    a.docs.erase(a.docs.begin() + i);   // VideoReader はデストラクタで停止
    if (a.docs.empty()) a.active = -1;
    else if (a.active >= (int)a.docs.size()) a.active = (int)a.docs.size() - 1;
    else if (i < a.active) a.active--;
}

static void doOpen(App& a)
{
    auto sel = pfd::open_file("音声ファイルを開く", ".",
        { "音声ファイル", "*.wav *.mp3 *.flac *.m4a *.aac *.ogg *.wma *.aif *.aiff", "すべて", "*" },
        pfd::opt::multiselect).result();
    if (!sel.empty()) openFiles(a, sel);
}

// カーソルがウィンドウを出入りしたら記録(離脱でコントロールバーを即座に隠す)
static void cursorEnterCallback(GLFWwindow* w, int entered)
{
    if (App* a = (App*)glfwGetWindowUserPointer(w))
        a->mouseInWindow = (entered != 0);
}

static void dropCallback(GLFWwindow* w, int count, const char** paths)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    if (!a || count <= 0) return;
    std::vector<std::string> v;
    for (int i = 0; i < count; i++) v.emplace_back(paths[i]);
    openFiles(*a, v);
}

// ---- 映像: 音声クロック同期 ----

// glGenerateMipmap は GL3.0 のためヘッダに無い。実行時に取得(取れなければmipmap無しで動作)。
#ifdef _WIN32
typedef void (__stdcall* PFN_glGenerateMipmap)(unsigned int);
#else
typedef void (*PFN_glGenerateMipmap)(unsigned int);
#endif
static PFN_glGenerateMipmap g_glGenerateMipmap = nullptr;

static void uploadVideoTexture(Doc& d, const VideoReader::Frame& f)
{
    if (!d.tex)
    {
        GLuint t = 0;
        glGenTextures(1, &t);
        d.tex = t;
        glBindTexture(GL_TEXTURE_2D, d.tex);
        // 縮小表示はミップマップ+trilinearでエイリアシングを防ぐ(無い環境はbilinear)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        g_glGenerateMipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, d.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, d.vidW, d.vidH, 0, GL_RGB, GL_UNSIGNED_BYTE, f.rgb.data());
    if (g_glGenerateMipmap) g_glGenerateMipmap(GL_TEXTURE_2D);
}

// 毎フレーム: 音声位置に合わせて映像フレームを進める。シーク/ループは自動で再スポーン。
static void updateVideo(App& a)
{
    Doc* d = curDoc(a);
    if (!d || !d->hasVideo) return;

    double audioT = d->playhead / (double)d->clip.sampleRate;

    bool needRestart = false;
    if (!d->vreader.running() && d->lastPts < 0) needRestart = true;                     // 初回/切替復帰
    else if (d->lastPts >= 0 && (audioT < d->lastPts - 0.3 || audioT > d->lastPts + 1.0))
        needRestart = true;                                                              // シーク/ループ検出

    if (needRestart)
    {
        d->vreader.start(d->clip.path, audioT, d->vidW, d->vidH, d->vidFps);
        d->wantPoster = true;
        d->lastPts = audioT;   // 再スポーン直後の再検出を防ぐ
    }

    VideoReader::Frame f;
    bool got = false;
    if (d->wantPoster)
    {
        got = d->vreader.popFirst(f);
        if (got) d->wantPoster = false;
    }
    else if (a.player.isPlaying())
    {
        got = d->vreader.popUpTo(audioT + 0.02, f);
    }

    if (got)
    {
        uploadVideoTexture(*d, f);
        d->lastPts = f.pts;
    }
}

// 全画面(モニターフルスクリーン)のトグル。YouTubeのダブルクリック/F/Escと同じ操作感。
static void toggleFullscreen(App& a)
{
    if (!a.window) return;
    if (!a.fullscreen)
    {
        glfwGetWindowPos(a.window, &a.savedX, &a.savedY);
        glfwGetWindowSize(a.window, &a.savedW, &a.savedH);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(a.window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
        a.fullscreen = true;
    }
    else
    {
        glfwSetWindowMonitor(a.window, nullptr, a.savedX, a.savedY, a.savedW, a.savedH, 0);
        a.fullscreen = false;
    }
}

static void togglePlay(App& a);   // 前方宣言(コントロールバーから使う)

static void fmtTime(double t, char* out, size_t n)
{
    int s = (int)(t + 0.5);
    if (s >= 3600) std::snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

// YouTube風コントロールバー(シークバー / 再生・一時停止 / 時間 / 全画面ボタン)
static void drawVideoControls(App& a, Doc& d, ImVec2 p0, ImVec2 p1)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float barH = 48.0f;
    ImVec2 b0(p0.x, p1.y - barH);

    // 下に向かって暗くなるグラデーション
    dl->AddRectFilledMultiColor(b0, p1,
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));

    long long frames = d.clip.frameCount();
    double dur = d.clip.duration();
    int sr = d.clip.sampleRate;

    // ---- シークバー ----
    float sx0 = p0.x + 14, sx1 = p1.x - 14;
    float sy = b0.y + 10;
    ImGui::SetCursorScreenPos(ImVec2(sx0, sy - 7));
    ImGui::SetNextItemAllowOverlap();   // トリムハンドルを上に重ねる
    ImGui::InvisibleButton("seek", ImVec2(sx1 - sx0, 14));
    bool seekHover = ImGui::IsItemHovered() || ImGui::IsItemActive();

    if (ImGui::IsItemActivated())
    {
        a.seekWasPlaying = a.player.isPlaying();
        a.player.stop();
    }
    if (ImGui::IsItemActive())
    {
        float t = std::clamp((ImGui::GetIO().MousePos.x - sx0) / std::max(1.0f, sx1 - sx0), 0.0f, 1.0f);
        d.playhead = (long long)(t * frames);
    }
    if (ImGui::IsItemDeactivated() && a.seekWasPlaying)
    {
        // 掴んで離したら続きから再生(YouTube方式)
        applyPlaybackGain(a);
        a.player.setLoop(a.loop);
        a.player.play(d.clip, d.playhead, frames, 0);
        a.seekWasPlaying = false;
    }

    float frac = frames > 0 ? (float)((double)d.playhead / frames) : 0.0f;
    float px = sx0 + frac * (sx1 - sx0);
    float th = seekHover ? 3.0f : 2.0f;
    dl->AddLine(ImVec2(sx0, sy), ImVec2(sx1, sy), IM_COL32(255, 255, 255, 70), th + 1);
    dl->AddLine(ImVec2(sx0, sy), ImVec2(px, sy), IM_COL32(78, 201, 176, 255), th + 1);
    dl->AddCircleFilled(ImVec2(px, sy), seekHover ? 7.0f : 5.0f, IM_COL32(78, 201, 176, 255));

    // ---- トリムモード: フォト風の開始/終了ハンドル ----
    if (a.videoTrimMode && frames > 0)
    {
        long long selS = (d.selStart >= 0 && d.selEnd > d.selStart) ? d.selStart : 0;
        long long selE = (d.selStart >= 0 && d.selEnd > d.selStart) ? d.selEnd : frames;
        long long minGap = std::max<long long>(1, frames / 200);   // 最小幅0.5%

        auto fToX = [&](long long f) { return sx0 + (float)((double)f / frames) * (sx1 - sx0); };
        auto xToF = [&](float x) {
            double t = std::clamp((x - sx0) / std::max(1.0f, sx1 - sx0), 0.0f, 1.0f);
            return (long long)(t * frames);
        };

        // 範囲外を暗く + 範囲を強調(トリム系はオレンジ: 再生位置のティール丸と区別)
        dl->AddRectFilled(ImVec2(sx0, sy - 5), ImVec2(fToX(selS), sy + 5), IM_COL32(0, 0, 0, 140));
        dl->AddRectFilled(ImVec2(fToX(selE), sy - 5), ImVec2(sx1, sy + 5), IM_COL32(0, 0, 0, 140));
        dl->AddLine(ImVec2(fToX(selS), sy), ImVec2(fToX(selE), sy), IM_COL32(242, 166, 76, 220), th + 3);

        // ハンドル(左右)。ドラッグで選択範囲を編集(既存の書き出し/切り出しにそのまま反映)
        struct HandleDef { const char* id; bool isStart; };
        for (const HandleDef& hd : { HandleDef{"trimL", true}, HandleDef{"trimR", false} })
        {
            long long f = hd.isStart ? selS : selE;
            float hx = fToX(f);
            ImGui::SetCursorScreenPos(ImVec2(hx - 7, sy - 15));
            ImGui::InvisibleButton(hd.id, ImVec2(14, 30));
            bool act = ImGui::IsItemActive();
            if (act)
            {
                long long nf = xToF(ImGui::GetIO().MousePos.x);
                if (hd.isStart) selS = std::min(nf, selE - minGap);
                else            selE = std::max(nf, selS + minGap);
                selS = std::max<long long>(0, selS);
                selE = std::min(selE, frames);
                d.selStart = selS;
                d.selEnd = selE;
            }
            ImU32 hcol = (act || ImGui::IsItemHovered()) ? IM_COL32(255, 200, 120, 255)
                                                         : IM_COL32(242, 166, 76, 255);
            float hx2 = fToX(hd.isStart ? selS : selE);
            dl->AddRectFilled(ImVec2(hx2 - 4, sy - 13), ImVec2(hx2 + 4, sy + 13), hcol, 3.0f);
            dl->AddLine(ImVec2(hx2, sy - 8), ImVec2(hx2, sy + 8), IM_COL32(60, 35, 10, 255), 1.5f);
        }
    }

    // ---- 再生/一時停止ボタン ----
    float by = b0.y + 22;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14, by));
    ImGui::InvisibleButton("pp", ImVec2(26, 24));
    if (ImGui::IsItemClicked()) togglePlay(a);
    ImU32 icoCol = IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 255 : 210);
    ImVec2 ic(p0.x + 14, by);
    if (a.player.isPlaying())
    {
        dl->AddRectFilled(ImVec2(ic.x + 4, ic.y + 3), ImVec2(ic.x + 10, ic.y + 21), icoCol);
        dl->AddRectFilled(ImVec2(ic.x + 15, ic.y + 3), ImVec2(ic.x + 21, ic.y + 21), icoCol);
    }
    else
    {
        dl->AddTriangleFilled(ImVec2(ic.x + 5, ic.y + 2), ImVec2(ic.x + 5, ic.y + 22),
                              ImVec2(ic.x + 22, ic.y + 12), icoCol);
    }

    // ---- 時間表示 ----
    char cur[16], tot[16], tim[40];
    fmtTime(d.playhead / (double)sr, cur, sizeof(cur));
    fmtTime(dur, tot, sizeof(tot));
    std::snprintf(tim, sizeof(tim), "%s / %s", cur, tot);
    dl->AddText(ImVec2(p0.x + 50, by + 3), IM_COL32(255, 255, 255, 220), tim);

    // ---- トリムボタン(フォト風ハンドルの表示切替) ----
    {
        float tx = p1.x - 40 - 62;
        ImGui::SetCursorScreenPos(ImVec2(tx, by));
        ImGui::InvisibleButton("trimbtn", ImVec2(54, 24));
        if (ImGui::IsItemClicked())
        {
            a.videoTrimMode = !a.videoTrimMode;
            if (!a.videoTrimMode) { d.selStart = d.selEnd = -1; }   // OFFで範囲解除
        }
        ImU32 tcol = a.videoTrimMode ? IM_COL32(242, 166, 76, 255)
                   : IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 255 : 210);
        dl->AddText(ImVec2(tx + 6, by + 3), tcol, "トリム");
    }

    // ---- 全画面ボタン(コーナー矢印アイコン) ----
    float fx = p1.x - 40;
    ImGui::SetCursorScreenPos(ImVec2(fx, by));
    ImGui::InvisibleButton("fsbtn", ImVec2(26, 24));
    if (ImGui::IsItemClicked()) toggleFullscreen(a);
    ImU32 fsCol = IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 255 : 210);
    {
        float cx = fx + 3, cy = by + 3, sz = 6.0f, W = 20.0f, H = 18.0f;
        float t2 = 2.0f;
        if (!a.fullscreen)
        {
            // 外向き: 四隅のブラケット
            dl->AddLine(ImVec2(cx, cy + sz), ImVec2(cx, cy), fsCol, t2);
            dl->AddLine(ImVec2(cx, cy), ImVec2(cx + sz, cy), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy), ImVec2(cx + W, cy), fsCol, t2);
            dl->AddLine(ImVec2(cx + W, cy), ImVec2(cx + W, cy + sz), fsCol, t2);
            dl->AddLine(ImVec2(cx, cy + H - sz), ImVec2(cx, cy + H), fsCol, t2);
            dl->AddLine(ImVec2(cx, cy + H), ImVec2(cx + sz, cy + H), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy + H), ImVec2(cx + W, cy + H), fsCol, t2);
            dl->AddLine(ImVec2(cx + W, cy + H), ImVec2(cx + W, cy + H - sz), fsCol, t2);
        }
        else
        {
            // 内向き: 復帰アイコン
            dl->AddLine(ImVec2(cx + sz, cy), ImVec2(cx + sz, cy + sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + sz, cy + sz), ImVec2(cx, cy + sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy), ImVec2(cx + W - sz, cy + sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy + sz), ImVec2(cx + W, cy + sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + sz, cy + H), ImVec2(cx + sz, cy + H - sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + sz, cy + H - sz), ImVec2(cx, cy + H - sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy + H), ImVec2(cx + W - sz, cy + H - sz), fsCol, t2);
            dl->AddLine(ImVec2(cx + W - sz, cy + H - sz), ImVec2(cx + W, cy + H - sz), fsCol, t2);
        }
    }
}

static void drawVideoPane(App& a, Doc& d, ImVec2 size)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    // 後から重ねるコントロール(シークバー等)に入力を通す。
    // これが無いと、先に置いたこの下地ボタンが全入力を奪ってバーが反応しない。
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("video", size);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // コントロールバーの表示判定:
    // カーソルが動いた直後は表示、静止2.5秒 or ペイン外で隠す(バー上に置きっぱなしでも隠す)。
    // シークバー/ハンドルのドラッグ中(IsAnyItemActive)は消さない。
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) a.lastMouseMove = now;
    bool hoverPane = ImGui::IsMouseHoveringRect(p0, p1);
    const float barH = 48.0f;
    bool inBarArea = hoverPane && io.MousePos.y > p1.y - barH;
    bool showCtl = a.mouseInWindow
                && (hoverPane || a.fullscreen)
                && (now - a.lastMouseMove < 2.5 || ImGui::IsAnyItemActive());

    // 全画面でバーが消えている間はマウスカーソルも隠す(YouTube方式)
    if (a.window)
        glfwSetInputMode(a.window, GLFW_CURSOR,
                         (a.fullscreen && !showCtl) ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);

    // クリックで再生/一時停止、ダブルクリックで全画面(いずれもバー領域は除外)
    bool inBar = showCtl && inBarArea;
    if (ImGui::IsItemHovered() && !inBar)
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            toggleFullscreen(a);
        else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            togglePlay(a);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));

    if (d.tex && d.vidW > 0 && d.vidH > 0)
    {
        // アスペクト維持でレターボックス
        float srcAspect = (float)d.vidW / d.vidH;
        float dstAspect = size.x / size.y;
        float w, h;
        if (srcAspect > dstAspect) { w = size.x; h = size.x / srcAspect; }
        else { h = size.y; w = size.y * srcAspect; }
        ImVec2 r0(p0.x + (size.x - w) * 0.5f, p0.y + (size.y - h) * 0.5f);
        ImVec2 r1(r0.x + w, r0.y + h);
        dl->AddImage((ImTextureID)(intptr_t)d.tex, r0, r1);
    }
    else
    {
        dl->AddText(ImVec2(p0.x + 14, p0.y + size.y * 0.5f - 9),
                    IM_COL32(150, 150, 150, 255), "映像を読み込み中…");
    }

    if (showCtl) drawVideoControls(a, d, p0, p1);

    // オーバーレイ配置でカーソルがずれるので、次のウィジェット(波形等)のために復元
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
}

static void drawWaveform(App& a, Doc& d, ImVec2 size)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("wave", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + size.x, p0.y + size.y);

    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 20, 255));

    long long frames = d.clip.frameCount();
    auto frameToX = [&](long long f) { return p0.x + (frames > 0 ? (float)((double)f / frames * size.x) : 0.0f); };
    auto xToFrame = [&](float x) {
        double v = (double)(x - p0.x) / std::max(1.0f, size.x) * frames;
        return (long long)std::clamp(v, 0.0, (double)frames);
    };

    int cols = std::max(1, (int)size.x);
    if (d.pkWidth != cols) computePeaks(d, cols);
    int lanes = std::max(1, d.pkLanes);

    if (d.selStart >= 0 && d.selEnd > d.selStart)
    {
        float x0 = frameToX(d.selStart), x1 = frameToX(d.selEnd);
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(std::max(x0 + 1, x1), p1.y), IM_COL32(78, 201, 176, 64));
    }

    // レーンごと(モノ=1段, ステレオ=L/R 2段)に波形を描く
    float laneH = size.y / lanes;
    int n = std::min(cols, (int)(d.pkMin.size() / lanes));
    for (int lane = 0; lane < lanes; lane++)
    {
        float mid = p0.y + laneH * lane + laneH * 0.5f;
        float halfH = laneH * 0.5f * 0.95f;
        dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), IM_COL32(64, 64, 68, 255));

        const float* mn = &d.pkMin[(size_t)(lane * cols)];
        const float* mx = &d.pkMax[(size_t)(lane * cols)];
        for (int c = 0; c < n; c++)
        {
            float x = p0.x + c + 0.5f;
            dl->AddLine(ImVec2(x, mid - mx[c] * halfH), ImVec2(x, mid - mn[c] * halfH),
                        IM_COL32(78, 201, 176, 255));
        }
    }
    if (lanes == 2)
    {
        // レーン境界とチャンネルラベル
        dl->AddLine(ImVec2(p0.x, p0.y + laneH), ImVec2(p1.x, p0.y + laneH), IM_COL32(45, 45, 50, 255));
        dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(140, 140, 145, 200), "L");
        dl->AddText(ImVec2(p0.x + 5, p0.y + laneH + 3), IM_COL32(140, 140, 145, 200), "R");
    }

    float phx = frameToX(d.playhead);
    dl->AddLine(ImVec2(phx, p0.y), ImVec2(phx, p1.y), IM_COL32(255, 208, 64, 255), 1.5f);

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) { d.dragging = true; d.downX = io.MousePos.x; }
    if (d.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        d.selStart = xToFrame(std::min(d.downX, io.MousePos.x));
        d.selEnd = xToFrame(std::max(d.downX, io.MousePos.x));
    }
    if (ImGui::IsItemDeactivated())
    {
        d.dragging = false;
        if (std::fabs(io.MousePos.x - d.downX) < 3.0f)
        {
            d.selStart = d.selEnd = -1;
            d.playhead = xToFrame(io.MousePos.x);
            a.player.stop();
        }
    }
}

static void handleShortcuts(App& a)
{
    // テキスト入力中 / ポップアップ(書き出し等)表示中はショートカット無効
    ImGuiIO& io = ImGui::GetIO();
    bool blocked = io.WantTextInput ||
                   ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (blocked)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && a.fullscreen) toggleFullscreen(a);
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay(a);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) goToStart(a);

    // 全画面: F で切替(映像タブのみ)、Esc で解除
    Doc* d = curDoc(a);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && d && d->hasVideo)
        toggleFullscreen(a);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && a.fullscreen)
        toggleFullscreen(a);
}

static void drawTabs(App& a)
{
    if (a.docs.empty()) return;
    int newActive = a.active;
    int toClose = -1;

    if (ImGui::BeginTabBar("docs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton))
    {
        for (int i = 0; i < (int)a.docs.size(); i++)
        {
            bool open = true;
            ImGui::PushID(i);
            // プログラム起因の選択は SetSelected で明示（ユーザークリックと区別する）
            ImGuiTabItemFlags fl = (i == a.pendingSelect) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(a.docs[i]->name.c_str(), &open, fl))
            {
                newActive = i;
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open) toClose = i;
        }
        ImGui::EndTabBar();
    }

    bool programmatic = (a.pendingSelect >= 0);
    if (newActive == a.pendingSelect) a.pendingSelect = -1;   // 選択が追いついたら解除
    if (newActive != a.active)
    {
        // 切替元の映像デコーダは止める(戻ってきたら再スポーン)
        if (Doc* od = curDoc(a); od && od->hasVideo)
        {
            od->vreader.stop();
            od->lastPts = -1;
        }
        a.active = newActive;
        if (!programmatic) startPlayback(a);  // ユーザーのタブ切替 → 切替先を即再生（排他）
    }
    if (toClose >= 0) closeDoc(a, toClose);
}

static void drawUI(App& a)
{
    handleShortcuts(a);
    Doc* d = curDoc(a);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // ---- 全画面モード: 映像のみを画面いっぱいに ----
    if (a.fullscreen && d && d->hasVideo)
    {
        drawVideoPane(a, *d, vp->WorkSize);
        ImGui::End();
        return;
    }
    if (a.fullscreen) toggleFullscreen(a);   // 映像タブでなくなったら自動復帰

    // ---- ツールバー ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.19f, 0.20f, 0.22f, 1.0f));
    ImGui::BeginChild("toolbar", ImVec2(0, 46), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(10, 8));
    ImGui::BeginGroup();
    if (ImGui::Button("開く")) doOpen(a);
    ImGui::SameLine();
    ImGui::BeginDisabled(d == nullptr);
    if (ImGui::Button("⏮ 先頭")) goToStart(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("再生位置を曲頭へ (Home)");
    ImGui::SameLine();
    const char* playLabel = a.player.isPlaying() ? "⏸ 一時停止" : "▶ 再生";
    if (ImGui::Button(playLabel)) togglePlay(a);
    ImGui::SameLine();
    if (ImGui::Button("■ 停止")) a.player.stop();
    ImGui::SameLine();
    if (ImGui::Checkbox("ループ", &a.loop)) a.player.setLoop(a.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115);
    if (ImGui::SliderFloat("##vol", &a.volumePct, 0.0f, 100.0f, "音量 %.0f%%"))
        applyPlaybackGain(a);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##stw", &a.stereoWidthPct, 0.0f, 200.0f, "ST幅 %.0f%%"))
        a.player.setStereoWidth(a.stereoWidthPct / 100.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ステレオ幅 (M/S): 0%%=モノラル 100%%=原音 200%%=ワイド\n再生のみ、書き出しには影響しません");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##monitor", kMonitors[(size_t)a.monitorIdx].name))
    {
        for (int i = 0; i < (int)kMonitors.size(); i++)
        {
            if (ImGui::Selectable(kMonitors[(size_t)i].name, i == a.monitorIdx))
            {
                a.monitorIdx = i;
                a.player.setMonitor(kMonitors[(size_t)i].specs, kMonitors[(size_t)i].mono);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("モニター: 再生音を各デバイスの聴こえ方でシミュレート\n(書き出し・解析には影響しません)");
    ImGui::SameLine();
    if (ImGui::Checkbox("-14 LUFSで再生", &a.normPlayback))
    {
        if (a.normPlayback && d && !d->loudValid) startMeasure(*d);
        applyPlaybackGain(a);
    }
    ImGui::SameLine();
    if (ImGui::Button("測定")) { if (d) { if (d->loudValid) showLoudness(*d); else startMeasure(*d); } }
    ImGui::SameLine();
    ImGui::Checkbox("解析", &a.showAnalysis);
    ImGui::SameLine();
    if (d && d->hasVideo)
    {
        ImGui::Checkbox("波形", &a.showVideoWave);
        ImGui::SameLine();
    }
    if (ImGui::Button("加工…")) a.wantFx = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(a.docs.size() < 2);
    if (ImGui::Button("連結…")) a.wantJoin = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("書き出し…")) a.wantExport = true;
    ImGui::SameLine();
    if (ImGui::Button("選択解除")) { if (d) d->selStart = d->selEnd = -1; }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- タブ ----
    drawTabs(a);
    d = curDoc(a);   // drawTabs でアクティブが変わり得る

    // ---- 波形 + 解析パネル ----
    const float statusH = 66.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float waveH = std::max(120.0f, avail.y - statusH);
    float panelW = (a.showAnalysis && d) ? 320.0f : 0.0f;
    if (d)
    {
        float mainW = avail.x - panelW;
        if (d->hasVideo)
        {
            // 映像 + (任意で)波形。波形はデフォルト非表示(ツールバーの「波形」でON)
            float wvH = a.showVideoWave ? std::min(170.0f, waveH * 0.45f) : 0.0f;
            float videoH = std::max(100.0f, waveH - wvH);
            ImGui::BeginGroup();
            drawVideoPane(a, *d, ImVec2(mainW, videoH));
            if (wvH > 0) drawWaveform(a, *d, ImVec2(mainW, waveH - videoH));
            ImGui::EndGroup();
        }
        else
        {
            drawWaveform(a, *d, ImVec2(mainW, waveH));
        }
        if (panelW > 0)
        {
            ImGui::SameLine(0, 0);
            drawAnalysisPanel(a, ImVec2(panelW, waveH));
        }
    }
    else
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("wave_empty", ImVec2(avail.x, waveH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + waveH), IM_COL32(18, 18, 20, 255));
        dl->AddText(ImVec2(p0.x + 14, p0.y + waveH * 0.5f - 9), IM_COL32(150, 150, 150, 255),
                    "音声ファイルを開く / ここにドラッグ&ドロップ（複数可）");
    }

    // ---- ステータス ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::BeginChild("status", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(10, 8));
    ImGui::BeginGroup();
    if (d)
    {
        char info[640];
        double pos = d->playhead / (double)d->clip.sampleRate;
        char sel[128] = "";
        if (d->selStart >= 0 && d->selEnd > d->selStart)
            std::snprintf(sel, sizeof(sel), "   選択 %.2f–%.2fs (%.2fs)",
                d->selStart / (double)d->clip.sampleRate, d->selEnd / (double)d->clip.sampleRate,
                (d->selEnd - d->selStart) / (double)d->clip.sampleRate);
        char norm[64] = "";
        if (a.normPlayback && d->loudValid)
            std::snprintf(norm, sizeof(norm), "   [再生 %+.1f dB]", d->playbackGainDb);
        char vid[64] = "";
        if (d->hasVideo)
            std::snprintf(vid, sizeof(vid), "   |   映像 %dx%d %.3g fps", d->srcW, d->srcH, d->vidFps);
        char bit[40] = "";
        if (!d->fmtDetail.empty()) std::snprintf(bit, sizeof(bit), " / %s", d->fmtDetail.c_str());
        std::snprintf(info, sizeof(info), "%s   |   %d Hz / %dch%s   |   長さ %.2fs   |   位置 %.2fs%s%s%s",
            d->name.c_str(), d->clip.sampleRate, d->clip.channels, bit, d->clip.duration(), pos, vid, sel, norm);
        ImGui::TextUnformatted(info);
        if (!d->loudText.empty())
            ImGui::TextColored(ImVec4(0.62f, 0.82f, 0.98f, 1.0f), "%s", d->loudText.c_str());
    }
    else
    {
        ImGui::TextUnformatted("ファイル未選択 — 「開く」/ ドラッグ&ドロップ（複数可）。Spaceで再生/停止。");
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- 書き出し/加工ダイアログ（モーダル）----
    if (a.wantExport) { ImGui::OpenPopup("export"); a.wantExport = false; }
    drawExportPopup(a);
    if (a.wantFx) { ImGui::OpenPopup("fx"); a.wantFx = false; }
    drawFxPopup(a);
    if (a.wantJoin) { ImGui::OpenPopup("join"); a.wantJoin = false; }
    drawJoinPopup(a);

    ImGui::End();
}

#ifdef _WIN32
// 2つ目のインスタンスから送られてきたファイルパスを受け取り、新規タブで開く
static WNDPROC g_prevWndProc = nullptr;
static App* g_app = nullptr;
static LRESULT CALLBACK PE_WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_COPYDATA)
    {
        COPYDATASTRUCT* cds = (COPYDATASTRUCT*)l;
        if (cds && cds->lpData && g_app)
        {
            std::wstring wp((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
            while (!wp.empty() && wp.back() == L'\0') wp.pop_back();
            openFiles(*g_app, { plat::wideToUtf8(wp) });
            ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        }
        return TRUE;
    }
    return CallWindowProcW(g_prevWndProc, h, msg, w, l);
}
#endif

static void glfw_error(int e, const char* d) { std::fprintf(stderr, "GLFW error %d: %s\n", e, d); }

int main(int argc, char** argv)
{
    std::string startupPath;
#ifdef _WIN32
    (void)argc; (void)argv;
    {
        int n = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
        if (wargv && n >= 2)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[1], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                std::string s(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wargv[1], -1, &s[0], len, nullptr, nullptr);
                startupPath = s;
            }
        }
        if (wargv) LocalFree(wargv);
    }

    // 単一インスタンス: 既に起動中なら、パスを既存ウィンドウへ渡して自分は終了
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"PlayerEditor_SingleInstance_v1");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(nullptr, L"PlayerEditor");
        if (other)
        {
            if (!startupPath.empty())
            {
                std::wstring wp = plat::utf8ToWide(startupPath);
                COPYDATASTRUCT cds{};
                cds.dwData = 1;
                cds.cbData = (DWORD)((wp.size() + 1) * sizeof(wchar_t));
                cds.lpData = (void*)wp.c_str();
                SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
            }
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
#else
    if (argc >= 2) startupPath = argv[1];
#endif

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    GLFWwindow* win = glfwCreateWindow(1330, 680, "PlayerEditor", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // ImGui のコールバック連鎖より先に登録(ImGui側が既存コールバックを呼び継ぐ)
    glfwSetCursorEnterCallback(win, cursorEnterCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    loadFont(io);
    ImGui::StyleColorsDark();

    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.94f, 0.95f, 1.0f);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
    st.WindowRounding = 0.0f;
    st.FrameRounding = 4.0f;
    st.WindowBorderSize = 0.0f;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // 映像テクスチャの縮小品質用(取得できない環境ではbilinearにフォールバック)
    g_glGenerateMipmap = (PFN_glGenerateMipmap)glfwGetProcAddress("glGenerateMipmap");

    App app;
    app.window = win;
    glfwSetWindowUserPointer(win, &app);
    glfwSetDropCallback(win, dropCallback);

#ifdef _WIN32
    g_app = &app;
    {
        HWND hwnd = glfwGetWin32Window(win);
        if (hwnd) g_prevWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)PE_WndProc);
    }
#endif

    if (!startupPath.empty()) openFiles(app, { startupPath });

    while (!glfwWindowShouldClose(win))
    {
        glfwPollEvents();
        app.player.update();
        if (Doc* d = curDoc(app))
        {
            if (app.player.isPlaying()) d->playhead = app.player.positionFrame();
            // 自然終了時: メインループのコピーが終端の少し手前で止まることがあるので
            // 正確に終端へスナップする(次の再生開始で「先頭に戻す」判定を確実に通す)
            if (app.player.takeJustFinished()) d->playhead = app.player.endFrame();
        }
        pollMeasure(app);
        updateVideo(app);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawUI(app);
        ImGui::Render();

        int w, h; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    app.player.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
