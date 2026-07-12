#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "miniaudio.h"
#include "biquad.h"

struct AudioClip;

// miniaudio による区間再生。位置は atomic で参照。
class Player
{
public:
    enum class State { Stopped, Playing, Paused };

    Player() = default;
    ~Player();

    // loopStartFrame: ループ時の戻り先(-1 = startFrame と同じ)。
    // 通常再生は曲頭(0)、選択範囲再生は選択先頭を渡す。
    void play(const AudioClip& clip, long long startFrame, long long endFrame,
              long long loopStartFrame = -1);
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

    // モニターシミュレーション(再生のみに掛かるフィルタ列。mono=モノラル合算)
    void setMonitor(const std::vector<Bq::Spec>& specs, bool mono);

    State state() const { return state_; }
    bool isPlaying() const { return state_ == State::Playing; }
    bool isPaused() const { return state_ == State::Paused; }
    long long positionFrame() const { return (long long)pos_.load(); }

private:
    static void dataCb(ma_device* d, void* out, const void* in, ma_uint32 count);
    void ensureDevice(int channels, int sampleRate);
    void rebuildMonitorLocked();   // monM_ 保持中に呼ぶ

    ma_device device_{};
    bool deviceInit_ = false;
    int devCh_ = 0, devSr_ = 0;

    const float* samples_ = nullptr;
    int channels_ = 0;
    std::atomic<uint64_t> pos_{0};
    uint64_t start_ = 0;
    uint64_t loopStart_ = 0;   // ループ時の戻り先
    uint64_t end_ = 0;
    std::atomic<bool> reachedEnd_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> loop_{false};
    State state_ = State::Stopped;

    // モニターチェーン(コールバックと共有、monM_ で保護)
    std::mutex monM_;
    std::vector<Bq::Spec> monSpecs_;
    std::vector<Bq::Biquad> monFilters_;
    bool monMono_ = false;
};
