#pragma once
#include <cmath>
#include <vector>

// タブ連結: A の末尾と B の先頭を等パワークロスフェードで繋ぐ(純関数、テスト可能)。
// 前提: A と B は同一チャンネル数・同一サンプルレート(呼び出し側で検証)。
namespace Join
{
    inline std::vector<float> crossfade(const std::vector<float>& a, const std::vector<float>& b,
                                        int ch, long long xfadeFrames)
    {
        long long fa = (long long)a.size() / ch;
        long long fb = (long long)b.size() / ch;
        long long xf = xfadeFrames;
        if (xf > fa) xf = fa;
        if (xf > fb) xf = fb;
        if (xf < 0) xf = 0;

        long long total = fa + fb - xf;
        std::vector<float> out((size_t)(total * ch));

        // A の非フェード部分
        for (long long f = 0; f < fa - xf; f++)
            for (int c = 0; c < ch; c++)
                out[(size_t)(f * ch + c)] = a[(size_t)(f * ch + c)];

        // クロスフェード部分 (等パワー: A=cos, B=sin)
        const double PI_2 = 1.57079632679489661923;
        for (long long f = 0; f < xf; f++)
        {
            double t = (xf > 1) ? (double)f / (double)(xf - 1) : 1.0;
            float gA = (float)std::cos(t * PI_2);
            float gB = (float)std::sin(t * PI_2);
            long long oa = (fa - xf + f) * ch;
            for (int c = 0; c < ch; c++)
                out[(size_t)(oa + c)] = a[(size_t)(oa + c)] * gA + b[(size_t)(f * ch + c)] * gB;
        }

        // B の残り
        for (long long f = xf; f < fb; f++)
            for (int c = 0; c < ch; c++)
                out[(size_t)((fa - xf + f) * ch + c)] = b[(size_t)(f * ch + c)];

        return out;
    }
}
