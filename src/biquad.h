#pragma once
#include <cmath>
#include <cstring>
#include <vector>

// RBJ Audio EQ Cookbook のバイクワッドフィルタ(純実装、テスト可能)。
namespace Bq
{
    constexpr int kMaxCh = 8;

    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[kMaxCh] = {}, z2[kMaxCh] = {};

        void reset() { std::memset(z1, 0, sizeof(z1)); std::memset(z2, 0, sizeof(z2)); }

        // Transposed Direct Form II
        inline float process(float x, int ch)
        {
            float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
    };

    inline Biquad highpass(double sr, double f, double Q)
    {
        double w = 2.0 * 3.14159265358979323846 * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * Q);
        double a0 = 1 + alpha;
        Biquad b;
        b.b0 = (float)(((1 + cw) / 2) / a0);
        b.b1 = (float)((-(1 + cw)) / a0);
        b.b2 = (float)(((1 + cw) / 2) / a0);
        b.a1 = (float)((-2 * cw) / a0);
        b.a2 = (float)((1 - alpha) / a0);
        return b;
    }

    inline Biquad lowpass(double sr, double f, double Q)
    {
        double w = 2.0 * 3.14159265358979323846 * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * Q);
        double a0 = 1 + alpha;
        Biquad b;
        b.b0 = (float)(((1 - cw) / 2) / a0);
        b.b1 = (float)((1 - cw) / a0);
        b.b2 = (float)(((1 - cw) / 2) / a0);
        b.a1 = (float)((-2 * cw) / a0);
        b.a2 = (float)((1 - alpha) / a0);
        return b;
    }

    inline Biquad peaking(double sr, double f, double Q, double gainDb)
    {
        double A = std::pow(10.0, gainDb / 40.0);
        double w = 2.0 * 3.14159265358979323846 * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * Q);
        double a0 = 1 + alpha / A;
        Biquad b;
        b.b0 = (float)((1 + alpha * A) / a0);
        b.b1 = (float)((-2 * cw) / a0);
        b.b2 = (float)((1 - alpha * A) / a0);
        b.a1 = (float)((-2 * cw) / a0);
        b.a2 = (float)((1 - alpha / A) / a0);
        return b;
    }

    inline Biquad lowShelf(double sr, double f, double Q, double gainDb)
    {
        double A = std::pow(10.0, gainDb / 40.0);
        double w = 2.0 * 3.14159265358979323846 * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * Q);
        double sq = 2.0 * std::sqrt(A) * alpha;
        double a0 = (A + 1) + (A - 1) * cw + sq;
        Biquad b;
        b.b0 = (float)((A * ((A + 1) - (A - 1) * cw + sq)) / a0);
        b.b1 = (float)((2 * A * ((A - 1) - (A + 1) * cw)) / a0);
        b.b2 = (float)((A * ((A + 1) - (A - 1) * cw - sq)) / a0);
        b.a1 = (float)((-2 * ((A - 1) + (A + 1) * cw)) / a0);
        b.a2 = (float)(((A + 1) + (A - 1) * cw - sq) / a0);
        return b;
    }

    // 周波数応答の振幅(dB)。テスト・可視化用。
    inline double magnitudeDbAt(const Biquad& b, double freqHz, double sr)
    {
        double w = 2.0 * 3.14159265358979323846 * freqHz / sr;
        // H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2), z = e^{jw}
        double c1 = std::cos(w), s1 = std::sin(w);
        double c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        double nr = b.b0 + b.b1 * c1 + b.b2 * c2;
        double ni = -(b.b1 * s1 + b.b2 * s2);
        double dr = 1.0 + b.a1 * c1 + b.a2 * c2;
        double di = -(b.a1 * s1 + b.a2 * s2);
        double num = std::sqrt(nr * nr + ni * ni);
        double den = std::sqrt(dr * dr + di * di);
        return 20.0 * std::log10(num / std::max(den, 1e-12));
    }

    // ---- モニターシミュレーション(デバイス別フィルタ仕様) ----
    enum class Type { Highpass, Lowpass, Peaking, LowShelf };
    struct Spec { Type type; float freq; float Q; float gainDb; };

    inline Biquad make(const Spec& s, double sr)
    {
        switch (s.type)
        {
            case Type::Highpass: return highpass(sr, s.freq, s.Q);
            case Type::Lowpass:  return lowpass(sr, s.freq, s.Q);
            case Type::Peaking:  return peaking(sr, s.freq, s.Q, s.gainDb);
            default:             return lowShelf(sr, s.freq, s.Q, s.gainDb);
        }
    }
}
