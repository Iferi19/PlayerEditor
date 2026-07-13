#pragma once
#include <cmath>
#include <vector>

// ジャンル別のモデルケース(参考値)。
// 注意: 実測データベースではなく、一般的なマスタリング傾向をモデル化した「目安」。
// スペクトラムは 1kHz を 0dB とした相対カーブ(10点、対数周波数で線形補間)。
namespace RefModel
{
    constexpr float kFreqs[10] = { 31.5f, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    struct Genre
    {
        const char* name;
        float corrMin, corrMax;     // 位相相関の目安レンジ
        float widthMin, widthMax;   // ステレオ幅(%)の目安レンジ
        float spec[10];             // 1kHz相対 dB
    };

    inline const std::vector<Genre>& genres()
    {
        static const std::vector<Genre> g = {
            { "ポップス",        0.4f, 0.90f,  60, 120, { +2, +4, +3, +2, +1, 0, -1, -3,  -7, -13 } },
            { "ロック",          0.3f, 0.80f,  70, 130, {  0, +3, +4, +3, +2, 0, -1, -3,  -8, -14 } },
            { "EDM",             0.3f, 0.80f,  80, 150, { +6, +7, +4, +2,  0, 0, -1, -3,  -6, -11 } },
            { "ヒップホップ",    0.5f, 0.95f,  40, 100, { +8, +8, +5, +2, +1, 0, -2, -5, -10, -17 } },
            { "ジャズ",          0.3f, 0.80f,  80, 140, {  0, +2, +2, +1, +1, 0, -2, -5, -11, -19 } },
            { "クラシック",      0.2f, 0.70f, 100, 160, { -2,  0, +1, +1,  0, 0, -3, -8, -14, -22 } },
            { "アコースティック", 0.4f, 0.90f,  50, 110, { -1, +1, +2, +1, +1, 0, -2, -5, -10, -17 } },
        };
        return g;
    }

    // 任意周波数のモデル値(dB, 1kHz相対)。対数周波数の線形補間、範囲外は端の値。
    inline float specAt(const Genre& g, float hz)
    {
        if (hz <= kFreqs[0]) return g.spec[0];
        if (hz >= kFreqs[9]) return g.spec[9];
        for (int i = 0; i < 9; i++)
        {
            if (hz <= kFreqs[i + 1])
            {
                float t = std::log(hz / kFreqs[i]) / std::log(kFreqs[i + 1] / kFreqs[i]);
                return g.spec[i] + t * (g.spec[i + 1] - g.spec[i]);
            }
        }
        return g.spec[9];
    }
}
