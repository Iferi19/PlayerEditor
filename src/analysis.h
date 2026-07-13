#pragma once
#include <cmath>
#include <cstdio>
#include <string>

// 解析レポートの生成(純関数、テスト可能)。
namespace Analysis
{
    struct Data
    {
        std::string file;
        double durationSec = 0;
        int sampleRate = 0;
        int channels = 0;
        long long frames = 0;
        double integratedLufs = NAN;   // NaN = 未測定
        double truePeakDbtp = NAN;
        double lraLu = NAN;
        double samplePeakDbfs = NAN;
        double rmsDbfs = NAN;
        double phaseCorrelation = NAN;   // -1..+1 (モノラルは NaN)
        double stereoWidthPct = NAN;     // 0..200 (モノラルは NaN)
    };

    inline std::string numOrNull(double v, const char* fmt = "%.2f")
    {
        if (std::isnan(v)) return "null";
        if (std::isinf(v)) return v < 0 ? "\"-inf\"" : "\"inf\"";
        char b[64];
        std::snprintf(b, sizeof(b), fmt, v);
        return b;
    }

    inline std::string jsonEscape(const std::string& s)
    {
        std::string r;
        for (char c : s)
        {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    }

    inline std::string toJson(const Data& d)
    {
        std::string j = "{\n";
        j += "  \"file\": \"" + jsonEscape(d.file) + "\",\n";
        j += "  \"duration_sec\": " + numOrNull(d.durationSec, "%.3f") + ",\n";
        j += "  \"sample_rate\": " + std::to_string(d.sampleRate) + ",\n";
        j += "  \"channels\": " + std::to_string(d.channels) + ",\n";
        j += "  \"frames\": " + std::to_string(d.frames) + ",\n";
        j += "  \"integrated_lufs\": " + numOrNull(d.integratedLufs) + ",\n";
        j += "  \"true_peak_dbtp\": " + numOrNull(d.truePeakDbtp) + ",\n";
        j += "  \"loudness_range_lu\": " + numOrNull(d.lraLu) + ",\n";
        j += "  \"sample_peak_dbfs\": " + numOrNull(d.samplePeakDbfs) + ",\n";
        j += "  \"rms_dbfs\": " + numOrNull(d.rmsDbfs) + ",\n";
        j += "  \"phase_correlation\": " + numOrNull(d.phaseCorrelation) + ",\n";
        j += "  \"stereo_width_pct\": " + numOrNull(d.stereoWidthPct, "%.1f") + ",\n";
        j += "  \"gain_to_minus14_lufs_db\": " +
             (std::isnan(d.integratedLufs) ? std::string("null") : numOrNull(-14.0 - d.integratedLufs)) + "\n";
        j += "}\n";
        return j;
    }

    inline std::string toText(const Data& d)
    {
        char b[1024];
        std::snprintf(b, sizeof(b),
            "File            : %s\n"
            "Duration        : %.3f s\n"
            "Sample rate     : %d Hz\n"
            "Channels        : %d\n"
            "Frames          : %lld\n"
            "Integrated      : %s LUFS\n"
            "True Peak       : %s dBTP\n"
            "Loudness Range  : %s LU\n"
            "Sample Peak     : %s dBFS\n"
            "RMS             : %s dBFS\n"
            "Phase Corr.     : %s\n"
            "Stereo Width    : %s %%\n"
            "Gain to -14LUFS : %s dB\n",
            d.file.c_str(), d.durationSec, d.sampleRate, d.channels, d.frames,
            numOrNull(d.integratedLufs, "%.1f").c_str(),
            numOrNull(d.truePeakDbtp, "%.1f").c_str(),
            numOrNull(d.lraLu, "%.1f").c_str(),
            numOrNull(d.samplePeakDbfs, "%.1f").c_str(),
            numOrNull(d.rmsDbfs, "%.1f").c_str(),
            numOrNull(d.phaseCorrelation, "%+.2f").c_str(),
            numOrNull(d.stereoWidthPct, "%.0f").c_str(),
            std::isnan(d.integratedLufs) ? "null" : numOrNull(-14.0 - d.integratedLufs, "%+.1f").c_str());
        return b;
    }
}
