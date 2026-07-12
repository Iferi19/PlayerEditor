#include "audio_clip.h"
#include "player.h"
#include "wav_io.h"
#include "ffmpeg.h"
#include "effects.h"
#include "analysis.h"
#include "join.h"
#include "spectrum.h"
#include "biquad.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

static int g_fail = 0;
static void check(const std::string& name, bool ok, const std::string& detail)
{
    std::printf("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (!ok) g_fail++;
}

int main()
{
    const std::string src = "D:/temp_Claude/MediaEditor/testtone.wav";

    // 1) miniaudio デコード
    AudioClip clip; std::string err;
    if (!AudioClip::load(src, clip, err)) { std::printf("load failed: %s\n", err.c_str()); return 2; }
    check("load: samplerate", clip.sampleRate == 44100, "sr=" + std::to_string(clip.sampleRate));
    check("load: channels", clip.channels == 2, "ch=" + std::to_string(clip.channels));
    check("load: frames", clip.frameCount() == 132300, "frames=" + std::to_string(clip.frameCount()));
    double peak = clip.samplePeakDb();
    check("load: sample peak ~ -6dB", std::fabs(peak - (-6.02)) < 0.2,
          "peak=" + std::to_string(peak));

    // 2) トリミング & ゲイン 書き出し → 再読込
    std::string trimPath = "D:/PlayerEditor/build/vx_trim.wav";
    check("trim: write", WavIo::writeFloatWav(trimPath, clip.samples, clip.channels, clip.sampleRate,
                                              22050, 44100, 0.0, err), err);
    AudioClip t;
    AudioClip::load(trimPath, t, err);
    check("trim: frames==22050", t.frameCount() == 22050, "frames=" + std::to_string(t.frameCount()));
    check("trim: peak preserved", std::fabs(t.samplePeakDb() - peak) < 0.05,
          "peak=" + std::to_string(t.samplePeakDb()));

    std::string gainPath = "D:/PlayerEditor/build/vx_gain.wav";
    WavIo::writeFloatWav(gainPath, clip.samples, clip.channels, clip.sampleRate, 0, clip.frameCount(), -6.0, err);
    AudioClip g;
    AudioClip::load(gainPath, g, err);
    check("gain: peak -6dB", std::fabs(g.samplePeakDb() - (peak - 6.0)) < 0.1,
          "peak=" + std::to_string(g.samplePeakDb()) + " (expect " + std::to_string(peak - 6.0) + ")");

    // 3) ffmpeg 測定
    if (Ffmpeg::available())
    {
        auto r = Ffmpeg::measure(src, err);
        check("ffmpeg: measure ok", r.ok, err);
        check("ffmpeg: integrated ~ -6.75", std::fabs(r.integratedLufs - (-6.75)) < 0.5,
              "I=" + std::to_string(r.integratedLufs) + " LUFS, TP=" + std::to_string(r.truePeakDb));
    }
    else
    {
        std::printf("[SKIP] ffmpeg not found\n");
    }

    // 4) miniaudio 再生: 位置が進む / 一時停止で保持
    {
        Player p;
        p.play(clip, 0, clip.frameCount());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        long long pos = p.positionFrame();
        check("player: playing", p.isPlaying(), "state ok");
        check("player: position advanced", pos > 1000,
              "pos=" + std::to_string(pos) + " (~" + std::to_string(pos / 44100.0) + "s)");
        p.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        long long held = p.positionFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check("player: paused holds position", std::llabs(p.positionFrame() - held) < 500,
              "held " + std::to_string(held) + " -> " + std::to_string(p.positionFrame()));
        p.stop();
    }

    // 5) 日本語ファイル名(Unicodeパス) の読み込み & 測定
    {
        const std::string jp = "D:/PlayerEditor/build/テスト音.wav";
        AudioClip jc;
        if (AudioClip::load(jp, jc, err))
        {
            check("unicode: load frames", jc.frameCount() == 132300, "frames=" + std::to_string(jc.frameCount()));
            if (Ffmpeg::available())
            {
                auto jr = Ffmpeg::measure(jp, err);
                check("unicode: ffmpeg measure", jr.ok, "I=" + std::to_string(jr.integratedLufs));
            }
        }
        else
        {
            std::printf("[SKIP] unicode file not present (%s)\n", err.c_str());
        }
    }

    // 6) ループ再生: 小区間をループ→終端を越えても再生継続＆位置が巻き戻る
    {
        Player p;
        p.setLoop(true);
        long long loopFrames = 4410;  // 0.1s
        p.play(clip, 0, loopFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(350));  // 0.1sを何周もする
        bool stillPlaying = p.isPlaying();
        long long pos = p.positionFrame();
        check("loop: still playing after end", stillPlaying, std::string("playing=") + (stillPlaying ? "1" : "0"));
        check("loop: position wrapped (< region end)", pos >= 0 && pos < loopFrames + 2205,
              "pos=" + std::to_string(pos) + " (region end=" + std::to_string(loopFrames) + ")");
        p.setLoop(false);
        p.stop();
    }

    // 6b) 途中から再生+ループ → 戻り先は曲頭(0)（再生開始地点ではない）
    {
        Player p;
        p.setLoop(true);
        // 0.5s 地点から 0.6s 地点まで再生、ループ戻り先=0
        p.play(clip, 22050, 26460, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(350));  // 終端到達→0へ巻き戻るはず
        long long pos = p.positionFrame();
        check("loop: wraps to track head (pos < play start)", p.isPlaying() && pos < 22050,
              "pos=" + std::to_string(pos) + " (started at 22050)");
        p.stop();
    }

    // 7) 多形式書き出し + タグ埋め込み
    if (Ffmpeg::available())
    {
        // まず float WAV を用意
        std::string tmp = "D:/PlayerEditor/build/pe_test_src.wav";
        WavIo::writeFloatWav(tmp, clip.samples, clip.channels, clip.sampleRate, 0, clip.frameCount(), 0.0, err);

        Tags tags;
        tags.title = "PE Test Title";
        tags.artist = "PE Artist";
        tags.album = "PE Album";

        const char* exts[] = { "mp3", "flac", "aif", "ogg", "m4a", "wav" };
        for (const char* ext : exts)
        {
            std::string out = std::string("D:/PlayerEditor/build/pe_out.") + ext;
            std::remove(out.c_str());
            bool ok = Ffmpeg::transcode(tmp, out, ext, tags, err);
            std::ifstream f(out, std::ios::binary | std::ios::ate);
            long long sz = f.good() ? (long long)f.tellg() : 0;
            check(std::string("encode ") + ext, ok && sz > 0,
                  ok ? ("size=" + std::to_string(sz)) : ("err=" + err));
        }

        // プリセット系: サンプルレート変換 + ビット深度指定
        {
            std::string o48 = "D:/PlayerEditor/build/pe_out48.wav";
            std::remove(o48.c_str());
            bool ok = Ffmpeg::transcode(tmp, o48, "wav", Tags{}, err, 48000, 16);
            AudioClip c48;
            bool loaded = ok && AudioClip::load(o48, c48, err);
            check("preset: wav 16bit/48k encodes", loaded, err);
            if (loaded)
            {
                check("preset: sr converted to 48000", c48.sampleRate == 48000,
                      "sr=" + std::to_string(c48.sampleRate));
                check("preset: duration preserved ~3s", std::fabs(c48.duration() - 3.0) < 0.02,
                      "dur=" + std::to_string(c48.duration()));
                check("preset: peak preserved ~ -6dB", std::fabs(c48.samplePeakDb() - (-6.02)) < 0.3,
                      "peak=" + std::to_string(c48.samplePeakDb()));
            }
            std::remove(o48.c_str());
        }

        // readTags: 書き出したファイルからタグを読み戻す(入力タグの流用経路)
        {
            Tags rt = Ffmpeg::readTags("D:/PlayerEditor/build/pe_out.flac");
            check("readTags flac: title", rt.title == "PE Test Title", "got=[" + rt.title + "]");
            check("readTags flac: artist", rt.artist == "PE Artist", "got=[" + rt.artist + "]");
            Tags rm = Ffmpeg::readTags("D:/PlayerEditor/build/pe_out.mp3");
            check("readTags mp3: album", rm.album == "PE Album", "got=[" + rm.album + "]");
        }

        // タグが実際に埋め込まれたか flac を ffprobe で確認
        std::string ff = Ffmpeg::findFfmpeg();
        std::string probe = ff.substr(0, ff.find_last_of("/\\") + 1) + "ffprobe.exe";
        std::ifstream pf(probe);
        if (pf.good())
        {
            std::string cmd = "\"\"" + probe + "\" -v quiet -show_entries format_tags=title -of default=nw=1 \"D:/PlayerEditor/build/pe_out.flac\"\"";
            std::string o;
            FILE* p = _popen(cmd.c_str(), "r");
            if (p) { char b[256]; size_t n; while ((n = fread(b, 1, sizeof(b), p)) > 0) o.append(b, n); _pclose(p); }
            check("flac tag embedded (title)", o.find("PE Test Title") != std::string::npos,
                  "ffprobe title=[" + o.substr(0, o.find_first_of("\r\n")) + "]");
        }
        std::remove(tmp.c_str());
    }

    // 8) エフェクト (純関数)
    {
        // gain: -6dB → ピークが -6dB 下がる
        std::vector<float> buf = clip.samples;
        Fx::gainDb(buf, clip.channels, 0, clip.frameCount(), -6.0);
        float pk = 0; for (float v : buf) pk = std::max(pk, std::fabs(v));
        double pkDb = 20.0 * std::log10(pk);
        check("fx gain: -6dB", std::fabs(pkDb - (peak - 6.0)) < 0.1, "peak=" + std::to_string(pkDb));

        // fadeIn: 先頭サンプルはほぼ0、フェード後は原音
        buf = clip.samples;
        long long fadeF = 4410;  // 0.1s
        Fx::fadeIn(buf, clip.channels, 0, clip.frameCount(), fadeF);
        float head = std::fabs(buf[0]);
        // フェード後の区間は未変更のはず
        bool tailSame = true;
        for (long long f = fadeF; f < fadeF + 1000; f++)
            for (int c = 0; c < clip.channels; c++)
                if (buf[(size_t)(f * clip.channels + c)] != clip.samples[(size_t)(f * clip.channels + c)])
                    { tailSame = false; break; }
        check("fx fadeIn: head silent", head < 1e-4f, "head=" + std::to_string(head));
        check("fx fadeIn: after fade untouched", tailSame, "");

        // fadeOut: 末尾サンプルはほぼ0
        buf = clip.samples;
        Fx::fadeOut(buf, clip.channels, 0, clip.frameCount(), fadeF);
        float tail = std::fabs(buf[buf.size() - 1]);
        check("fx fadeOut: tail silent", tail < 1e-3f, "tail=" + std::to_string(tail));

        // reverse: 2回反転で元に戻る
        buf = clip.samples;
        Fx::reverse(buf, clip.channels, 0, clip.frameCount());
        bool changed = false;
        for (size_t i = 0; i < 2000 && !changed; i++) if (buf[i] != clip.samples[i]) changed = true;
        Fx::reverse(buf, clip.channels, 0, clip.frameCount());
        bool restored = true;
        for (size_t i = 0; i < buf.size(); i++) if (buf[i] != clip.samples[i]) { restored = false; break; }
        check("fx reverse: changes data", changed, "");
        check("fx reverse: double reverse restores", restored, "");
    }

    // 9) 解析: RMS と レポート生成
    {
        // -6.02dBFS の正弦波 → RMS はさらに -3.01dB ≈ -9.03dBFS
        double rms = clip.rmsDb();
        check("analysis: rms of -6dB sine ~ -9.03", std::fabs(rms - (-9.03)) < 0.1,
              "rms=" + std::to_string(rms));

        Analysis::Data ad;
        ad.file = "test\"quote.wav";   // エスケープ確認
        ad.durationSec = clip.duration();
        ad.sampleRate = clip.sampleRate;
        ad.channels = clip.channels;
        ad.frames = clip.frameCount();
        ad.integratedLufs = -6.75;
        ad.truePeakDbtp = -6.02;
        ad.lraLu = 0.0;
        ad.samplePeakDbfs = peak;
        ad.rmsDbfs = rms;

        std::string j = Analysis::toJson(ad);
        check("analysis json: has integrated", j.find("\"integrated_lufs\": -6.75") != std::string::npos, "");
        check("analysis json: gain to -14", j.find("\"gain_to_minus14_lufs_db\": -7.25") != std::string::npos, "");
        check("analysis json: escaped quote", j.find("test\\\"quote.wav") != std::string::npos, "");

        Analysis::Data nod;   // 未測定(NaN) → null になるか
        nod.file = "x.wav";
        std::string j2 = Analysis::toJson(nod);
        check("analysis json: unmeasured -> null", j2.find("\"integrated_lufs\": null") != std::string::npos, "");

        std::string t = Analysis::toText(ad);
        check("analysis text: has LUFS line", t.find("-6.8 LUFS") != std::string::npos || t.find("-6.7 LUFS") != std::string::npos, "");
    }

    // 10) タブ連結 (等パワークロスフェード)
    {
        int ch = 2;
        long long fa = 1000, fb = 1000, xf = 200;
        std::vector<float> A((size_t)(fa * ch), 1.0f);
        std::vector<float> B((size_t)(fb * ch), 1.0f);
        auto J = Join::crossfade(A, B, ch, xf);

        check("join: length = A+B-xf", (long long)J.size() == (fa + fb - xf) * ch,
              "len=" + std::to_string(J.size() / ch) + " expect " + std::to_string(fa + fb - xf));
        // 非フェード部は素通し(=1.0)
        check("join: head untouched", std::fabs(J[0] - 1.0f) < 1e-6f, "");
        check("join: tail untouched", std::fabs(J[J.size() - 1] - 1.0f) < 1e-6f, "");
        // フェード中央: cos(45°)+sin(45°) = √2 ≈ 1.414 (等パワーの根拠)
        long long midF = fa - xf + xf / 2;
        float mid = J[(size_t)(midF * ch)];
        check("join: equal-power center ~ 1.414", std::fabs(mid - 1.4142f) < 0.02f,
              "mid=" + std::to_string(mid));
        // フェード端: 始端=A側1.0, 終端=B側1.0
        float xs = J[(size_t)((fa - xf) * ch)];
        float xe = J[(size_t)((fa - 1) * ch)];
        check("join: xfade start ~1.0", std::fabs(xs - 1.0f) < 0.02f, "start=" + std::to_string(xs));
        check("join: xfade end ~1.0", std::fabs(xe - 1.0f) < 0.02f, "end=" + std::to_string(xe));

        // xf=0 (単純連結)
        auto J0 = Join::crossfade(A, B, ch, 0);
        check("join: xf=0 simple concat", (long long)J0.size() == (fa + fb) * ch, "");
    }

    // 11) スペクトラム (FFT精度)
    {
        // testtone = 440Hz, 振幅0.5(-6.02dBFS) ステレオ
        const int FFTN = 4096;
        int sr = clip.sampleRate;
        auto mags = Spec::magnitudesDb(clip.samples, clip.channels, clip.frameCount(),
                                       clip.frameCount() / 2, FFTN);

        // ピークbinの周波数 ≈ 440Hz (分解能 sr/N ≈ 10.8Hz)
        int argmax = 0;
        for (int k = 1; k < (int)mags.size(); k++) if (mags[(size_t)k] > mags[(size_t)argmax]) argmax = k;
        double peakHz = (double)argmax * sr / FFTN;
        check("spec: peak bin ~440Hz", std::fabs(peakHz - 440.0) < 2.0 * sr / FFTN,
              "peak=" + std::to_string(peakHz) + " Hz");

        // ピークレベル ≈ -6dBFS (窓のスキャロッピングで最大-1.4dB落ちる)
        check("spec: peak level ~ -6dBFS", mags[(size_t)argmax] > -8.5 && mags[(size_t)argmax] < -5.0,
              "level=" + std::to_string(mags[(size_t)argmax]));

        // 4.4kHz(10倍上)のbinは 30dB 以上低い
        int farBin = (int)(4400.0 * FFTN / sr);
        check("spec: 4.4kHz bin much lower", mags[(size_t)farBin] < mags[(size_t)argmax] - 30.0,
              "far=" + std::to_string(mags[(size_t)farBin]));

        // 帯域集計: 最大の帯域は 440Hz を含む帯域
        const int NB = 30;
        auto bands = Spec::bandLevelsDb(mags, sr, FFTN, NB, 20.0f, 20000.0f);
        int bmax = 0;
        for (int b = 1; b < NB; b++) if (bands[(size_t)b] > bands[(size_t)bmax]) bmax = b;
        float f0 = 20.0f * std::pow(1000.0f, (float)bmax / NB);        // 20*(20000/20)^(b/NB)
        float f1 = 20.0f * std::pow(1000.0f, (float)(bmax + 1) / NB);
        check("spec: loudest band contains 440Hz", f0 <= 440.0f && 440.0f <= f1,
              "band=" + std::to_string(f0) + "-" + std::to_string(f1) + " Hz");

        // 曲全体の平均スペクトラム: 定常音なので瞬時値とほぼ同じはず
        auto avg = Spec::averageSpectrumDb(clip.samples, clip.channels, clip.frameCount(), FFTN);
        int aMax = 0;
        for (int k = 1; k < (int)avg.size(); k++) if (avg[(size_t)k] > avg[(size_t)aMax]) aMax = k;
        double aPeakHz = (double)aMax * sr / FFTN;
        check("spec avg: peak ~440Hz", std::fabs(aPeakHz - 440.0) < 2.0 * sr / FFTN,
              "peak=" + std::to_string(aPeakHz) + " Hz");
        check("spec avg: level ~ -6dBFS", avg[(size_t)aMax] > -8.5 && avg[(size_t)aMax] < -5.0,
              "level=" + std::to_string(avg[(size_t)aMax]));

        // 低域の補間: binごとに値が異なるランプを細かくリサンプルしても階段(連続同値)にならない
        {
            std::vector<float> ramp((size_t)(FFTN / 2));
            for (int k = 0; k < FFTN / 2; k++) ramp[(size_t)k] = (float)k * 0.5f;  // bin=kで k/2 dB
            // 20-200Hz (bin 1.9〜18.6 付近) を200点で: 補間が無いと大量の同値が並ぶ
            auto rs = Spec::bandLevelsDb(ramp, sr, FFTN, 200, 20.0f, 200.0f);
            int dup = 0;
            for (int i = 1; i < 200; i++) if (rs[(size_t)i] == rs[(size_t)(i - 1)]) dup++;
            check("spec interp: low band has no staircase", dup < 20,
                  "duplicates=" + std::to_string(dup) + "/199");
            // 単調増加(ランプなので)
            bool mono = true;
            for (int i = 1; i < 200; i++) if (rs[(size_t)i] < rs[(size_t)(i - 1)] - 0.01f) { mono = false; break; }
            check("spec interp: monotonic on ramp", mono, "");
        }
    }

    // 12) バイクワッドフィルタ(モニターシミュレーション)
    {
        double sr = 44100;

        // ハイパス500Hz: 通過帯域はフラット、低域は大きく減衰
        auto hp = Bq::highpass(sr, 500, 0.707);
        check("bq hp: flat at 2kHz", std::fabs(Bq::magnitudeDbAt(hp, 2000, sr)) < 0.5,
              std::to_string(Bq::magnitudeDbAt(hp, 2000, sr)) + " dB");
        check("bq hp: -25dB+ at 100Hz", Bq::magnitudeDbAt(hp, 100, sr) < -25.0,
              std::to_string(Bq::magnitudeDbAt(hp, 100, sr)) + " dB");

        // ピーキング +6dB@1kHz: 中心で+6、離れた帯域はフラット
        auto pk = Bq::peaking(sr, 1000, 1.0, 6.0);
        check("bq peak: +6dB at 1kHz", std::fabs(Bq::magnitudeDbAt(pk, 1000, sr) - 6.0) < 0.3,
              std::to_string(Bq::magnitudeDbAt(pk, 1000, sr)) + " dB");
        check("bq peak: flat at 50Hz", std::fabs(Bq::magnitudeDbAt(pk, 50, sr)) < 0.5,
              std::to_string(Bq::magnitudeDbAt(pk, 50, sr)) + " dB");

        // ローシェルフ +6dB@100Hz: 低域+6、高域フラット
        auto ls = Bq::lowShelf(sr, 100, 0.9, 6.0);
        check("bq shelf: +6dB at 20Hz", std::fabs(Bq::magnitudeDbAt(ls, 20, sr) - 6.0) < 1.0,
              std::to_string(Bq::magnitudeDbAt(ls, 20, sr)) + " dB");
        check("bq shelf: flat at 2kHz", std::fabs(Bq::magnitudeDbAt(ls, 2000, sr)) < 0.5,
              std::to_string(Bq::magnitudeDbAt(ls, 2000, sr)) + " dB");

        // 実信号処理: 440Hz正弦波をHP2kHzに通す → 理論減衰量と一致
        auto hp2k = Bq::highpass(sr, 2000, 0.707);
        double predicted = Bq::magnitudeDbAt(hp2k, 440, sr);
        int n = 8820;   // 0.2s
        float peakOut = 0;
        for (int i = 0; i < n; i++)
        {
            float x = 0.5f * (float)std::sin(2 * 3.14159265358979 * 440 * i / sr);
            float y = hp2k.process(x, 0);
            if (i > 4410) peakOut = std::max(peakOut, std::fabs(y));   // 整定後
        }
        double measured = 20.0 * std::log10(peakOut / 0.5);
        check("bq process: matches frequency response", std::fabs(measured - predicted) < 1.0,
              "measured=" + std::to_string(measured) + " predicted=" + std::to_string(predicted));
    }

    std::printf("\n%s\n", g_fail == 0 ? "=> ALL PASS" : ("=> " + std::to_string(g_fail) + " FAILED").c_str());
    return g_fail == 0 ? 0 : 1;
}
