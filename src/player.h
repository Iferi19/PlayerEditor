#pragma once
#include <atomic>
#include <cstdint>
#include "miniaudio.h"

struct AudioClip;

// miniaudio による区間再生。位置は atomic で参照。
class Player
{
public:
    enum class State { Stopped, Playing, Paused };

    Player() = default;
    ~Player();

    void play(const AudioClip& clip, long long startFrame, long long endFrame);
    void pause();
    void resume();
    void stop();
    void update();   // UI から毎フレーム呼ぶ: 終端で自動停止

    // 再生時ゲイン(線形)。1.0=素通し。-14 LUFS再生モードで使う。
    void setGain(float g) { gain_.store(g); }
    float gain() const { return gain_.load(); }

    // ループ再生
    void setLoop(bool v) { loop_.store(v); }
    bool loop() const { return loop_.load(); }

    State state() const { return state_; }
    bool isPlaying() const { return state_ == State::Playing; }
    bool isPaused() const { return state_ == State::Paused; }
    long long positionFrame() const { return (long long)pos_.load(); }

private:
    static void dataCb(ma_device* d, void* out, const void* in, ma_uint32 count);
    void ensureDevice(int channels, int sampleRate);

    ma_device device_{};
    bool deviceInit_ = false;
    int devCh_ = 0, devSr_ = 0;

    const float* samples_ = nullptr;
    int channels_ = 0;
    std::atomic<uint64_t> pos_{0};
    uint64_t start_ = 0;
    uint64_t end_ = 0;
    std::atomic<bool> reachedEnd_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> loop_{false};
    State state_ = State::Stopped;
};
