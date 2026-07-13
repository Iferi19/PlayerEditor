#pragma once
#include <cmath>
#include <vector>

// ステレオ解析(純関数、テスト可能)。
// - 位相相関: L/R の相関係数 (+1=完全一致(モノ), 0=無相関, -1=逆相=打ち消しの危険)
// - ステレオ幅: Mid/Side エネルギー比 (0%=モノ, 100%=M/S同等, 200%=純サイド)
namespace Stereo
{
    struct Meter
    {
        double correlation = 1.0;   // -1 .. +1
        double widthPct = 0.0;      // 0 .. 200
    };

    // 区間 [f0, f1) を解析(ステレオ以上は先頭2chを使用)
    inline Meter measure(const std::vector<float>& s, int ch, long long f0, long long f1)
    {
        Meter m;
        if (ch < 2) return m;   // モノラルは相関+1/幅0のまま

        long long frames = (long long)s.size() / ch;
        if (f0 < 0) f0 = 0;
        if (f1 > frames) f1 = frames;
        if (f1 <= f0) return m;

        double sumLR = 0, sumLL = 0, sumRR = 0, sumMM = 0, sumSS = 0;
        for (long long f = f0; f < f1; f++)
        {
            double L = s[(size_t)(f * ch)];
            double R = s[(size_t)(f * ch + 1)];
            sumLR += L * R;
            sumLL += L * L;
            sumRR += R * R;
            double mid = 0.5 * (L + R);
            double sd = 0.5 * (L - R);
            sumMM += mid * mid;
            sumSS += sd * sd;
        }

        const double eps = 1e-12;
        if (sumLL < eps && sumRR < eps)
        {
            m.correlation = 1.0;   // 無音はモノ互換とみなす
            m.widthPct = 0.0;
            return m;
        }
        double den = std::sqrt(sumLL * sumRR);
        m.correlation = den < eps ? 0.0 : sumLR / den;
        if (m.correlation > 1.0) m.correlation = 1.0;
        if (m.correlation < -1.0) m.correlation = -1.0;

        double mRms = std::sqrt(sumMM);
        double sRms = std::sqrt(sumSS);
        m.widthPct = (mRms + sRms) < eps ? 0.0 : 200.0 * sRms / (mRms + sRms);
        return m;
    }
}
