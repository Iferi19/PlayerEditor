#pragma once
#include <string>
#include <vector>

// メモリ上に展開した音声。samples は [-1,1] 正規化 float のインターリーブ。
struct AudioClip
{
    std::vector<float> samples;
    int channels = 0;
    int sampleRate = 0;
    std::string path;

    long long frameCount() const { return channels > 0 ? (long long)samples.size() / channels : 0; }
    double duration() const { return sampleRate > 0 ? (double)frameCount() / sampleRate : 0.0; }
    double samplePeakDb() const;
    double rmsDb() const;   // 全体RMS(dBFS)。無音は -inf。

    // wav/mp3/flac 等を miniaudio でデコード。成功で true。
    static bool load(const std::string& path, AudioClip& out, std::string& err);
};
