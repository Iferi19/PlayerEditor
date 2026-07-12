#pragma once
#include <string>

struct LoudnessResult
{
    double integratedLufs = 0;
    double truePeakDb = 0;
    double loudnessRange = 0;
    bool ok = false;
};

// 楽曲情報（埋め込みメタデータ）
struct Tags
{
    std::string title, artist, album, albumArtist, genre, year, track;
    bool any() const
    {
        return !(title.empty() && artist.empty() && album.empty() && albumArtist.empty()
                 && genre.empty() && year.empty() && track.empty());
    }
};

namespace Ffmpeg
{
    const std::string& findFfmpeg();   // 見つからなければ ""
    const std::string& findFfprobe();
    bool available();
    LoudnessResult measure(const std::string& input, std::string& err);

    // 入力ファイルに埋め込まれた曲情報を読む(ffprobe)。無ければ空のまま。
    Tags readTags(const std::string& input);

    // float WAV(inWav) を fmt("wav"/"mp3"/"m4a"/"flac"/"ogg"/"aif") にエンコードし、
    // 対応形式なら tags を埋め込む。成功で true。
    // outSampleRate: 0=維持 / それ以外=リサンプル。bitDepth: 0=形式の既定 / 16 / 24 / 32(wavのみfloat)。
    bool transcode(const std::string& inWav, const std::string& outPath,
                   const std::string& fmt, const Tags& tags, std::string& err,
                   int outSampleRate = 0, int bitDepth = 0);
}
