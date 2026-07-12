#pragma once
#include <string>

struct LoudnessResult
{
    double integratedLufs = 0;
    double truePeakDb = 0;
    double loudnessRange = 0;
    bool ok = false;
};

namespace Ffmpeg
{
    const std::string& findFfmpeg();   // 見つからなければ ""
    bool available();
    LoudnessResult measure(const std::string& input, std::string& err);
}
