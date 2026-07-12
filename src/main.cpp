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

// 1タブ = 1ファイル分の状態
struct Doc
{
    AudioClip clip;
    std::string name;   // タブ表示名(ファイル名)

    long long playhead = 0;
    long long selStart = -1, selEnd = -1;

    std::vector<float> pkMin, pkMax;
    int pkWidth = -1;

    LoudnessResult loud;
    bool loudValid = false;
    std::string loudText;
    double playbackGainDb = 0.0;

    std::shared_ptr<MeasureState> ms = std::make_shared<MeasureState>();
    long long measureGen = 0;
    bool measuring = false;

    Tags tags;   // 書き出し時に埋め込む曲情報

    // 解析パネル用キャッシュ(NaN=未計算。加工で無効化)
    double peakCache = NAN;
    double rmsCache = NAN;

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
    int pendingSelect = -1;     // プログラム起因のタブ選択(切替時に停止させない)
    bool showAnalysis = false;  // 解析サイドパネル表示

    // 書き出し設定
    int exportFmt = 0;              // kFormats のインデックス
    bool exportUseSelection = false;
    bool exportNormalize = false;
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
    d.pkMin.assign(cols, 0.0f);
    d.pkMax.assign(cols, 0.0f);
    const auto& s = d.clip.samples;
    int ch = d.clip.channels;
    long long frames = d.clip.frameCount();

    for (int c = 0; c < cols; c++)
    {
        long long f0 = (long long)((double)c / cols * frames);
        long long f1 = (long long)((double)(c + 1) / cols * frames);
        if (f1 <= f0) f1 = f0 + 1;
        float mn = 1.0f, mx = -1.0f;
        for (long long f = f0; f < f1 && f < frames; f++)
        {
            float v = 0.0f;
            const float* p = &s[(size_t)f * ch];
            for (int k = 0; k < ch; k++) v += p[k];
            v /= ch;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (mn > mx) { mn = 0.0f; mx = 0.0f; }
        d.pkMin[c] = mn;
        d.pkMax[c] = mx;
    }
    d.pkWidth = cols;
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

    double gain = 0.0;
    if (a.exportNormalize)
    {
        if (!ensureMeasuredSync(*d)) return;   // ffmpegで測定
        gain = -14.0 - d->loud.integratedLufs;
    }

    const char* ext = kExts[a.exportFmt];
    std::string nm = d->name;
    auto dot = nm.find_last_of('.');
    if (dot != std::string::npos) nm = nm.substr(0, dot);
    std::string suggested = nm + (a.exportNormalize ? "_-14LUFS" : "") + (useSel ? "_trim" : "") + "." + ext;

    auto out = pfd::save_file("書き出し", suggested,
        { std::string(kFormats[a.exportFmt]) + " (*." + ext + ")", std::string("*.") + ext }).result();
    if (out.empty()) return;

    // 一時的に float WAV を作り、ffmpeg で目的形式へエンコード（タグ埋め込み）
    std::string tmp = tempWavPath();
    std::string err;
    if (!WavIo::writeFloatWav(tmp, d->clip.samples, d->clip.channels, d->clip.sampleRate, s, e, gain, err))
    { d->loudText = "書き出し失敗: " + err; return; }

    bool ok = Ffmpeg::transcode(tmp, out, ext, d->tags, err);
    std::remove(tmp.c_str());

    if (ok)
    {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "書き出し完了: %s  [%s%s%s]",
            baseName(out).c_str(), kFormats[a.exportFmt],
            a.exportNormalize ? " / -14 LUFS" : "", useSel ? " / 選択範囲" : "");
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

// 解析パネル/保存で使うキャッシュを確保(ピーク/RMSは全走査なので一度だけ計算)
static void ensureAnalysisCache(Doc& d)
{
    if (std::isnan(d.peakCache)) d.peakCache = d.clip.samplePeakDb();
    if (std::isnan(d.rmsCache)) d.rmsCache = d.clip.rmsDb();
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
        { "JSON", "*.json", "テキスト", "*.txt" }).result();
    if (out.empty()) return;

    bool asText = out.size() > 4 && out.substr(out.size() - 4) == ".txt";
    std::ofstream f(out, std::ios::binary);
    if (!f) { d->loudText = "解析結果の保存に失敗しました。"; return; }
    f << (asText ? Analysis::toText(ad) : Analysis::toJson(ad));
    d->loudText = "解析結果を保存しました: " + baseName(out);
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

    ImGui::TextUnformatted("範囲:");
    ImGui::SameLine();
    if (ImGui::RadioButton("全体", !a.exportUseSelection)) a.exportUseSelection = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::RadioButton("選択範囲", a.exportUseSelection)) a.exportUseSelection = true;
    ImGui::EndDisabled();
    if (!hasSel) a.exportUseSelection = false;

    ImGui::Checkbox("-14 LUFS に正規化して書き出す", &a.exportNormalize);
    ImGui::Combo("形式", &a.exportFmt, kFormats, IM_ARRAYSIZE(kFormats));

    ImGui::SeparatorText("曲情報（タグ）");
    ImGui::InputText("タイトル", &d->tags.title);
    ImGui::InputText("アーティスト", &d->tags.artist);
    ImGui::InputText("アルバム", &d->tags.album);
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
    auto [s, e] = region(*d);
    bool hasSel = (d->selStart >= 0 && d->selEnd > d->selStart);
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
    a.docs.erase(a.docs.begin() + i);
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

static void dropCallback(GLFWwindow* w, int count, const char** paths)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    if (!a || count <= 0) return;
    std::vector<std::string> v;
    for (int i = 0; i < count; i++) v.emplace_back(paths[i]);
    openFiles(*a, v);
}

static void drawWaveform(App& a, Doc& d, ImVec2 size)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("wave", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + size.x, p0.y + size.y);

    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 20, 255));
    float mid = p0.y + size.y * 0.5f;
    float halfH = size.y * 0.5f;
    dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), IM_COL32(64, 64, 68, 255));

    long long frames = d.clip.frameCount();
    auto frameToX = [&](long long f) { return p0.x + (frames > 0 ? (float)((double)f / frames * size.x) : 0.0f); };
    auto xToFrame = [&](float x) {
        double v = (double)(x - p0.x) / std::max(1.0f, size.x) * frames;
        return (long long)std::clamp(v, 0.0, (double)frames);
    };

    int cols = std::max(1, (int)size.x);
    if (d.pkWidth != cols) computePeaks(d, cols);

    if (d.selStart >= 0 && d.selEnd > d.selStart)
    {
        float x0 = frameToX(d.selStart), x1 = frameToX(d.selEnd);
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(std::max(x0 + 1, x1), p1.y), IM_COL32(78, 201, 176, 64));
    }

    int n = std::min(cols, (int)d.pkMin.size());
    for (int c = 0; c < n; c++)
    {
        float x = p0.x + c + 0.5f;
        dl->AddLine(ImVec2(x, mid - d.pkMax[c] * halfH), ImVec2(x, mid - d.pkMin[c] * halfH),
                    IM_COL32(78, 201, 176, 255));
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
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay(a);
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

    // ---- ツールバー ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.19f, 0.20f, 0.22f, 1.0f));
    ImGui::BeginChild("toolbar", ImVec2(0, 46), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(10, 8));
    ImGui::BeginGroup();
    if (ImGui::Button("開く")) doOpen(a);
    ImGui::SameLine();
    ImGui::BeginDisabled(d == nullptr);
    const char* playLabel = a.player.isPlaying() ? "⏸ 一時停止" : "▶ 再生";
    if (ImGui::Button(playLabel)) togglePlay(a);
    ImGui::SameLine();
    if (ImGui::Button("■ 停止")) a.player.stop();
    ImGui::SameLine();
    if (ImGui::Checkbox("ループ", &a.loop)) a.player.setLoop(a.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    if (ImGui::SliderFloat("##vol", &a.volumePct, 0.0f, 100.0f, "音量 %.0f%%"))
        applyPlaybackGain(a);
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
    float panelW = (a.showAnalysis && d) ? 280.0f : 0.0f;
    if (d)
    {
        drawWaveform(a, *d, ImVec2(avail.x - panelW, waveH));
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
        std::snprintf(info, sizeof(info), "%s   |   %d Hz / %dch   |   長さ %.2fs   |   位置 %.2fs%s%s",
            d->name.c_str(), d->clip.sampleRate, d->clip.channels, d->clip.duration(), pos, sel, norm);
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
    GLFWwindow* win = glfwCreateWindow(1060, 680, "PlayerEditor", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

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

    App app;
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
            if (app.player.isPlaying()) d->playhead = app.player.positionFrame();
        pollMeasure(app);

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
