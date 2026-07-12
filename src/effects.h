#pragma once
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <vector>

// メモリ上のインターリーブ float サンプルに対する破壊的エフェクト。
// frames はフレーム数(チャンネル数で割った長さ)、範囲は [startFrame, endFrame)。
namespace Fx
{
    inline void gainDb(std::vector<float>& s, int ch, long long startFrame, long long endFrame, double db)
    {
        float g = (float)std::pow(10.0, db / 20.0);
        long long i0 = startFrame * ch, i1 = endFrame * ch;
        i1 = std::min<long long>(i1, (long long)s.size());
        for (long long i = i0; i < i1; i++) s[(size_t)i] *= g;
    }

    // 範囲の先頭から fadeFrames かけて 0 -> 1 の線形フェードイン
    inline void fadeIn(std::vector<float>& s, int ch, long long startFrame, long long endFrame, long long fadeFrames)
    {
        long long n = std::min(fadeFrames, endFrame - startFrame);
        for (long long f = 0; f < n; f++)
        {
            float g = (float)f / (float)n;
            float* p = &s[(size_t)((startFrame + f) * ch)];
            for (int c = 0; c < ch; c++) p[c] *= g;
        }
    }

    // 範囲の末尾に向かって fadeFrames かけて 1 -> 0 の線形フェードアウト
    inline void fadeOut(std::vector<float>& s, int ch, long long startFrame, long long endFrame, long long fadeFrames)
    {
        long long n = std::min(fadeFrames, endFrame - startFrame);
        for (long long f = 0; f < n; f++)
        {
            float g = (float)(n - 1 - f) / (float)n;
            float* p = &s[(size_t)((endFrame - n + f) * ch)];
            for (int c = 0; c < ch; c++) p[c] *= g;
        }
    }

    // 範囲をフレーム単位で逆順に(チャンネル内容は保持)
    inline void reverse(std::vector<float>& s, int ch, long long startFrame, long long endFrame)
    {
        long long a = startFrame, b = endFrame - 1;
        while (a < b)
        {
            float* pa = &s[(size_t)(a * ch)];
            float* pb = &s[(size_t)(b * ch)];
            for (int c = 0; c < ch; c++) std::swap(pa[c], pb[c]);
            a++; b--;
        }
    }
}
