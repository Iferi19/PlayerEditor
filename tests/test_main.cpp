#include "audio_clip.h"
#include "player.h"
#include "wav_io.h"
#include "ffmpeg.h"
#include "effects.h"
#include "analysis.h"

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

    std::printf("\n%s\n", g_fail == 0 ? "=> ALL PASS" : ("=> " + std::to_string(g_fail) + " FAILED").c_str());
    return g_fail == 0 ? 0 : 1;
}
