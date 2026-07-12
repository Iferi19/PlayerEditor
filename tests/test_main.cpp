#include "audio_clip.h"
#include "player.h"
#include "wav_io.h"
#include "ffmpeg.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

static int g_fail = 0;
static void check(const char* name, bool ok, const std::string& detail)
{
    std::printf("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
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

    std::printf("\n%s\n", g_fail == 0 ? "=> ALL PASS" : ("=> " + std::to_string(g_fail) + " FAILED").c_str());
    return g_fail == 0 ? 0 : 1;
}
