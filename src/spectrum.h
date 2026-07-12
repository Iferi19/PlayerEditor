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

    // binスペクトルを対数間隔の帯域にまとめる(帯域内はパワー平均、dBで返す)
    // nBands を描画ピクセル数にすれば連続曲線用の対数リサンプルとしても使える。
    inline std::vector<float> bandLevelsDb(const std::vector<float>& magsDb, int sampleRate,
                                           int fftSize, int nBands, float fMin, float fMax)
    {
        std::vector<float> out((size_t)nBands, -120.0f);
        float nyquist = sampleRate * 0.5f;
        fMax = std::min(fMax, nyquist);
        for (int b = 0; b < nBands; b++)
        {
            float f0 = fMin * std::pow(fMax / fMin, (float)b / nBands);
            float f1 = fMin * std::pow(fMax / fMin, (float)(b + 1) / nBands);
            double k0f = (double)f0 * fftSize / sampleRate;
            double k1f = (double)f1 * fftSize / sampleRate;

            if (k1f - k0f >= 1.5)
            {
                // 帯域が複数binを含む: パワー平均
                int k0 = std::max(1, (int)std::floor(k0f));
                int k1 = std::max(k0 + 1, (int)std::ceil(k1f));
                k1 = std::min(k1, fftSize / 2);
                if (k0 >= k1) continue;
                double acc = 0.0;
                for (int k = k0; k < k1; k++)
                {
                    double lin = std::pow(10.0, magsDb[(size_t)k] / 20.0);
                    acc += lin * lin;
                }
                acc /= (k1 - k0);
                out[(size_t)b] = (float)(10.0 * std::log10(std::max(acc, 1e-14)));
            }
            else
            {
                // 帯域がbinより細かい(低域): 隣接binをパワー領域で線形補間して階段を除去
                double kc = std::sqrt((double)f0 * f1) * fftSize / sampleRate;   // 帯域中心(幾何平均)
                int k = (int)kc;
                k = std::max(1, std::min(k, fftSize / 2 - 2));
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
