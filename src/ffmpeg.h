#pragma once
#include <string>
#include <vector>

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

struct StreamInfo
{
    bool hasAudio = false;
    int sampleRate = 0, channels = 0;
    int audioBits = 0;          // PCM系のビット深度(0=非PCM/不明)
    std::string codecName;      // pcm_s24le / mp3 / aac / flac ...
    int bitRateKbps = 0;        // 0=不明(主に非PCM用)
    bool hasVideo = false;
    int width = 0, height = 0;
    double fps = 0;
};

struct PcmData
{
    std::vector<float> samples;   // インターリーブ f32
    int channels = 0;
    int sampleRate = 0;
};

namespace Ffmpeg
{
    const std::string& findFfmpeg();   // 見つからなければ ""
    const std::string& findFfprobe();
    bool available();
    LoudnessResult measure(const std::string& input, std::string& err);

    // ffprobe でストリーム情報(音声sr/ch、映像w/h/fps)を取得
    StreamInfo probe(const std::string& input);

    // 先頭音声ストリームを f32 PCM にデコード(動画コンテナやminiaudio未対応形式用)
    bool decodeAudio(const std::string& input, PcmData& out, std::string& err);

    // 映像+音声を再エンコードなしで [t0,t1) 秒で切り出し(キーフレーム精度)
    bool cutVideoCopy(const std::string& input, const std::string& outPath,
                      double t0, double t1, std::string& err);

    // 入力ファイルに埋め込まれた曲情報を読む(ffprobe)。無ければ空のまま。
    Tags readTags(const std::string& input);

    // float WAV(inWav) を fmt("wav"/"mp3"/"m4a"/"flac"/"ogg"/"aif") にエンコードし、
    // 対応形式なら tags を埋め込む。成功で true。
    // outSampleRate: 0=維持 / それ以外=リサンプル。bitDepth: 0=形式の既定 / 16 / 24 / 32(wavのみfloat)。
    bool transcode(const std::string& inWav, const std::string& outPath,
                   const std::string& fmt, const Tags& tags, std::string& err,
                   int outSampleRate = 0, int bitDepth = 0);
}
