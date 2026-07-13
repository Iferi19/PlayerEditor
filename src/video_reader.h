#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ffmpeg を常駐デコーダとして起動し、rawvideo(RGB24) をパイプで受け取るストリーミングリーダー。
// 再生は音声がマスタークロック。UIは pts <= 現在時刻 のフレームを取り出して表示する。
// シークは stop() → start(t) で再スポーン。
class VideoReader
{
public:
    struct Frame
    {
        std::vector<uint8_t> rgb;   // outW*outH*3
        double pts = 0;             // 秒(ソース基準)
    };

    ~VideoReader();

    // t 秒位置から outW x outH のRGB24ストリームを開始。fps はコンテナの値。
    bool start(const std::string& path, double t, int outW, int outH, double fps);
    void stop();
    bool running() const { return running_; }
    double startTime() const { return startT_; }

    // pts <= maxPts の最新フレームを取得(それより古いものは捨てる)。無ければ false。
    bool popUpTo(double maxPts, Frame& out);
    // 先頭のフレームを1枚取得(ポスター用、ptsは問わない)。
    bool popFirst(Frame& out);

private:
    void readLoop(size_t frameSize, double t, double fps);

    std::thread thread_;
    std::mutex m_;
    std::deque<Frame> queue_;
    bool running_ = false;
    bool stopFlag_ = false;
    double startT_ = -1;

#ifdef _WIN32
    void* proc_ = nullptr;   // HANDLE
    void* pipe_ = nullptr;   // HANDLE (読み取り側)
#else
    void* pipe_ = nullptr;   // FILE*
#endif
};
