#include "wav_io.h"
#include <cmath>
#include <cstdint>
#include <fstream>

namespace
{
    template <class T>
    void w(std::ofstream& f, T v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
}

bool WavIo::writeFloatWav(const std::string& path, const std::vector<float>& interleaved,
                          int channels, int sampleRate, long long startFrame, long long endFrame,
                          double gainDb, std::string& err)
{
    long long startIdx = startFrame * channels;
    long long endIdx = endFrame * channels;
    if (endIdx > (long long)interleaved.size()) endIdx = (long long)interleaved.size();
    if (endIdx <= startIdx) { err = "書き出す範囲が空です。"; return false; }

    long long nSamples = endIdx - startIdx;
    uint32_t dataBytes = (uint32_t)(nSamples * 4);
    float g = (gainDb == 0.0) ? 1.0f : (float)std::pow(10.0, gainDb / 20.0);

    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "ファイルを作成できませんでした。"; return false; }

    f.write("RIFF", 4);
    w<uint32_t>(f, 36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w<uint32_t>(f, 16);
    w<uint16_t>(f, 3);                               // IEEE float
    w<uint16_t>(f, (uint16_t)channels);
    w<uint32_t>(f, (uint32_t)sampleRate);
    w<uint32_t>(f, (uint32_t)(sampleRate * channels * 4)); // byte rate
    w<uint16_t>(f, (uint16_t)(channels * 4));        // block align
    w<uint16_t>(f, 32);                              // bits
    f.write("data", 4);
    w<uint32_t>(f, dataBytes);

    if (g == 1.0f)
    {
        f.write(reinterpret_cast<const char*>(interleaved.data() + startIdx), (std::streamsize)dataBytes);
    }
    else
    {
        for (long long i = startIdx; i < endIdx; i++)
        {
            float v = interleaved[(size_t)i] * g;
            w<float>(f, v);
        }
    }
    return (bool)f;
}
