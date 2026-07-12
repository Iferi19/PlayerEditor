// PlayerEditor — C++ / Dear ImGui / GLFW / miniaudio / ffmpeg
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "portable-file-dialogs.h"

#include "audio_clip.h"
#include "player.h"
#include "wav_io.h"
#include "ffmpeg.h"

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
    if (a.normPlayback && d && d->loudValid)
    {
        d->playbackGainDb = -14.0 - d->loud.integratedLufs;
        a.player.setGain((float)std::pow(10.0, d->playbackGainDb / 20.0));
    }
    else
    {
        if (d) d->playbackGainDb = 0.0;
        a.player.setGain(1.0f);
    }
}

static void startMeasure(Doc& d)
{
    if (d.measuring) return;
    if (!Ffmpeg::available()) { d.loudText = "ffmpeg が見つかりません（winget等で導入してください）。"; return; }

    d.measuring = true;
    long long gen = ++d.measureGen;
    d.loudText = "測定中… (バックグラウンド)";

    auto ms = d.ms;
    std::string path = d.clip.path;
    std::thread([ms, path, gen]()
    {
        std::string err;
        LoudnessResult r = Ffmpeg::measure(path, err);
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
    std::string err;
    auto r = Ffmpeg::measure(d.clip.path, err);
    if (!r.ok) { d.loudText = "測定失敗: " + err; return false; }
    d.loud = r; d.loudValid = true; showLoudness(d);
    return true;
}

static void doNormalize(App& a)
{
    Doc* d = curDoc(a);
    if (!d || !ensureMeasuredSync(*d)) return;
    double gain = -14.0 - d->loud.integratedLufs;
    auto out = pfd::save_file("正規化して書き出し", suggestName(*d, "_-14LUFS"), { "WAV", "*.wav" }).result();
    if (out.empty()) return;

    std::string err;
    if (!WavIo::writeFloatWav(out, d->clip.samples, d->clip.channels, d->clip.sampleRate,
                              0, d->clip.frameCount(), gain, err))
    { d->loudText = "書き出し失敗: " + err; return; }

    auto check = Ffmpeg::measure(out, err);
    char buf[512];
    if (check.ok)
        std::snprintf(buf, sizeof(buf), "書き出し完了 (Gain %+.1f dB / 適用後 %.1f LUFS, TP %.1f dBTP)",
                      gain, check.integratedLufs, check.truePeakDb);
    else
        std::snprintf(buf, sizeof(buf), "書き出し完了 (Gain %+.1f dB)", gain);
    d->loudText = buf;
}

static void doTrim(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    if (!(d->selStart >= 0 && d->selEnd > d->selStart))
    { d->loudText = "波形上をドラッグして範囲を選択してください。"; return; }

    auto out = pfd::save_file("選択範囲を書き出し", suggestName(*d, "_trim"), { "WAV", "*.wav" }).result();
    if (out.empty()) return;

    std::string err;
    if (!WavIo::writeFloatWav(out, d->clip.samples, d->clip.channels, d->clip.sampleRate,
                              d->selStart, d->selEnd, 0.0, err))
    { d->loudText = "書き出し失敗: " + err; return; }

    double dur = (d->selEnd - d->selStart) / (double)d->clip.sampleRate;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "トリミング書き出し完了 (%.2f s)", dur);
    d->loudText = buf;
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

static void togglePlay(App& a)
{
    Doc* d = curDoc(a);
    if (!d) return;
    if (a.player.isPlaying()) a.player.stop();
    else if (a.player.isPaused()) a.player.resume();
    else { applyPlaybackGain(a); a.player.setLoop(a.loop); auto [s, e] = region(*d); a.player.play(d->clip, s, e); }
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
    a.docs.push_back(std::move(doc));
    a.active = (int)a.docs.size() - 1;
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

    if (ImGui::BeginTabBar("docs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable |
                                   ImGuiTabBarFlags_TabListPopupButton))
    {
        for (int i = 0; i < (int)a.docs.size(); i++)
        {
            bool open = true;
            ImGui::PushID(i);
            if (ImGui::BeginTabItem(a.docs[i]->name.c_str(), &open, ImGuiTabItemFlags_None))
            {
                newActive = i;
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open) toClose = i;
        }
        ImGui::EndTabBar();
    }

    if (newActive != a.active) { a.active = newActive; a.player.stop(); }  // タブ切替で停止（排他）
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
    if (ImGui::Checkbox("-14 LUFSで再生", &a.normPlayback))
    {
        if (a.normPlayback && d && !d->loudValid) startMeasure(*d);
        applyPlaybackGain(a);
    }
    ImGui::SameLine();
    if (ImGui::Button("測定")) { if (d) { if (d->loudValid) showLoudness(*d); else startMeasure(*d); } }
    ImGui::SameLine();
    if (ImGui::Button("正規化書き出し(-14)")) doNormalize(a);
    ImGui::SameLine();
    if (ImGui::Button("選択範囲を書き出し")) doTrim(a);
    ImGui::SameLine();
    if (ImGui::Button("選択解除")) { if (d) d->selStart = d->selEnd = -1; }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- タブ ----
    drawTabs(a);
    d = curDoc(a);   // drawTabs でアクティブが変わり得る

    // ---- 波形 ----
    const float statusH = 66.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float waveH = std::max(120.0f, avail.y - statusH);
    if (d)
    {
        drawWaveform(a, *d, ImVec2(avail.x, waveH));
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
