#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// スペクトラム解析(純関数、テスト可能)。radix-2 FFT + Hann窓 + 対数バンド集計。
namespace Spec
{
    inline void fft(std::vector<std::complex<float>>& x)   // in-place, size は2の冪
    {
        const size_t n = x.size();
        for (size_t i = 1, j = 0; i < n; i++)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            float ang = -6.28318530717958647692f / (float)len;
            std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; k++)
                {
                    auto u = x[i + k];
                    auto v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // centerFrame 周辺 fftSize フレームをモノ化 + Hann窓 → binごとの振幅(dBFS)。
    // 校正: フルスケール正弦波(振幅1.0)のピークbinが ≈ 0 dBFS になるよう補正済み。
    inline std::vector<float> magnitudesDb(const std::vector<float>& samples, int ch,
                                           long long frames, long long centerFrame, int fftSize)
    {
        std::vector<std::complex<float>> buf((size_t)fftSize);
        long long start = centerFrame - fftSize / 2;
        for (int i = 0; i < fftSize; i++)
        {
            long long f = start + i;
            float v = 0.0f;
            if (f >= 0 && f < frames)
            {
                const float* p = &samples[(size_t)(f * ch)];
                for (int c = 0; c < ch; c++) v += p[c];
                v /= ch;
            }
            float w = 0.5f * (1.0f - std::cos(6.28318530717958647692f * i / (fftSize - 1)));
            buf[(size_t)i] = v * w;
        }
        fft(buf);

        // Hann のコヒーレントゲイン 0.5、片側スペクトルの2倍 → 除数 N/4
        std::vector<float> out((size_t)(fftSize / 2));
        float norm = (float)fftSize * 0.25f;
        for (int k = 0; k < fftSize / 2; k++)
        {
            float m = std::abs(buf[(size_t)k]) / norm;
            out[(size_t)k] = 20.0f * std::log10(std::max(m, 1e-7f));
        }
        return out;
    }

    // 曲全体の平均スペクトラム(Welch法・重なり無し、パワー平均)。binごとのdBを返す。
    inline std::vector<float> averageSpectrumDb(const std::vector<float>& samples, int ch,
                                                long long frames, int fftSize)
    {
        std::vector<double> acc((size_t)(fftSize / 2), 0.0);
        int n = 0;
        if (frames < fftSize)
        {
            auto m = magnitudesDb(samples, ch, frames, frames / 2, fftSize);
            for (int k = 0; k < fftSize / 2; k++) acc[(size_t)k] += std::pow(10.0, m[(size_t)k] / 10.0);
            n = 1;
        }
        else
        {
            for (long long start = 0; start + fftSize <= frames; start += fftSize)
            {
                auto m = magnitudesDb(samples, ch, frames, start + fftSize / 2, fftSize);
                for (int k = 0; k < fftSize / 2; k++) acc[(size_t)k] += std::pow(10.0, m[(size_t)k] / 10.0);
                n++;
            }
        }
        std::vector<float> out((size_t)(fftSize / 2));
        for (int k = 0; k < fftSize / 2; k++)
            out[(size_t)k] = (float)(10.0 * std::log10(std::max(acc[(size_t)k] / n, 1e-14)));
        return out;
    }

    // 定Q(フラクショナルオクターブ)スムージングで対数間隔にリサンプル。
    // 各出力点をその中心周波数に比例した幅(±octaveHalf オクターブ)で平均するため、
    // 低域の角張り(bin間を直線で結ぶギザギザ)が自然にならされる。プロのアナライザー方式。
    // nBands=描画ピクセル数にすれば連続曲線になる。
    inline std::vector<float> bandLevelsDb(const std::vector<float>& magsDb, int sampleRate,
                                           int fftSize, int nBands, float fMin, float fMax,
                                           float octaveHalf = 1.0f / 6.0f)   // 1/6oct半幅=1/3oct全幅
    {
        std::vector<float> out((size_t)nBands, -120.0f);
        int nb = fftSize / 2;
        float nyquist = sampleRate * 0.5f;
        fMax = std::min(fMax, nyquist);
        float loMul = std::pow(2.0f, -octaveHalf);
        float hiMul = std::pow(2.0f, +octaveHalf);

        for (int b = 0; b < nBands; b++)
        {
            // 出力点の中心周波数(対数軸で等間隔・各セルの中心)
            float fc = fMin * std::pow(fMax / fMin, (b + 0.5f) / nBands);
            double k0 = std::max(1.0, (double)(fc * loMul) * fftSize / sampleRate);
            double k1 = std::min((double)nb, (double)(fc * hiMul) * fftSize / sampleRate);

            if (k1 - k0 >= 1.0)
            {
                // スムージング窓のパワー平均。bin k は周波数 index=k を中心に [k-0.5,k+0.5] を占める
                // とみなし、窓の端を fractional重み で加重(fcが動くと値も連続=階段が出ない)。
                int a = std::max(1, (int)std::floor(k0 + 0.5));
                int c = std::min(nb - 1, (int)std::ceil(k1 - 0.5));
                double acc = 0.0, wsum = 0.0;
                for (int k = a; k <= c; k++)
                {
                    double lo = std::max((double)k - 0.5, k0);
                    double hi = std::min((double)k + 0.5, k1);
                    double w = hi - lo;   // このbinが窓に占める割合
                    if (w <= 0.0) continue;
                    acc += std::pow(10.0, magsDb[(size_t)k] / 10.0) * w;   // パワー×重み
                    wsum += w;
                }
                out[(size_t)b] = (float)(10.0 * std::log10(std::max(acc / std::max(wsum, 1e-9), 1e-14)));
            }
            else
            {
                // 窓がbinより狭い(最低域): 隣接binをパワー領域で線形補間
                double kc = (double)fc * fftSize / sampleRate;
                int k = std::max(1, std::min((int)kc, nb - 2));
                double frac = std::clamp(kc - k, 0.0, 1.0);
                double p0 = std::pow(10.0, magsDb[(size_t)k] / 10.0);
                double p1 = std::pow(10.0, magsDb[(size_t)(k + 1)] / 10.0);
                double p = p0 * (1.0 - frac) + p1 * frac;
                out[(size_t)b] = (float)(10.0 * std::log10(std::max(p, 1e-14)));
            }
        }
        return out;
    }
}
