#include "audio_clip.h"
#include "miniaudio.h"
#include "platform_utf8.h"
#include <cmath>

double AudioClip::samplePeakDb() const
{
    float peak = 0.0f;
    for (float s : samples)
    {
        float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    if (peak <= 0.0f) return -INFINITY;
    return 20.0 * std::log10((double)peak);
}

bool AudioClip::load(const std::string& path, AudioClip& out, std::string& err)
{
    // ネイティブのch/レートを維持(0指定)、フォーマットは f32
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder dec;
#ifdef _WIN32
    ma_result initR = ma_decoder_init_file_w(plat::utf8ToWide(path).c_str(), &cfg, &dec);
#else
    ma_result initR = ma_decoder_init_file(path.c_str(), &cfg, &dec);
#endif
    if (initR != MA_SUCCESS)
    {
        err = "デコードを初期化できませんでした（未対応の形式かもしれません）。";
        return false;
    }

    out.channels = (int)dec.outputChannels;
    out.sampleRate = (int)dec.outputSampleRate;
    out.path = path;
    out.samples.clear();

    const ma_uint64 CHUNK = 8192;
    std::vector<float> tmp((size_t)CHUNK * out.channels);
    for (;;)
    {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, tmp.data(), CHUNK, &got);
        if (got > 0)
            out.samples.insert(out.samples.end(), tmp.begin(), tmp.begin() + (size_t)got * out.channels);
        if (r != MA_SUCCESS || got < CHUNK) break;
    }
    ma_decoder_uninit(&dec);

    if (out.samples.empty() || out.channels <= 0 || out.sampleRate <= 0)
    {
        err = "サンプルを読み取れませんでした。";
        return false;
    }
    return true;
}
