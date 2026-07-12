#pragma once
#include <string>
#include <vector>

namespace WavIo
{
    // [startFrame,endFrame) を float32 WAV で書き出す。gainDb を掛けられる。
    bool writeFloatWav(const std::string& path, const std::vector<float>& interleaved,
                       int channels, int sampleRate, long long startFrame, long long endFrame,
                       double gainDb, std::string& err);
}
