#include "player.h"
#include "audio_clip.h"

Player::~Player()
{
    if (deviceInit_) ma_device_uninit(&device_);
}

void Player::dataCb(ma_device* d, void* out, const void* /*in*/, ma_uint32 count)
{
    Player* p = (Player*)d->pUserData;
    float* o = (float*)out;
    int ch = p->channels_;
    uint64_t pos = p->pos_.load();
    float g = p->gain_.load();

    // モニターチェーンはUIスレッドから差し替わるため保護(切替時のみ競合)
    std::lock_guard<std::mutex> lk(p->monM_);
    bool hasMon = !p->monFilters_.empty() || p->monMono_;

    bool loop = p->loop_.load();
    float fr[Bq::kMaxCh];
    for (ma_uint32 i = 0; i < count; i++)
    {
        if (p->samples_ && pos >= p->end_ && loop)
            pos = p->loopStart_;   // ループ: 戻り先へ(通常=曲頭, 選択=選択先頭)

        if (p->samples_ && pos < p->end_)
        {
            const float* src = p->samples_ + pos * ch;
            if (hasMon && ch <= Bq::kMaxCh)
            {
                for (int c = 0; c < ch; c++) fr[c] = src[c];
                for (auto& f : p->monFilters_)
                    for (int c = 0; c < ch; c++) fr[c] = f.process(fr[c], c);
                if (p->monMono_ && ch > 1)
                {
                    float m = 0;
                    for (int c = 0; c < ch; c++) m += fr[c];
                    m /= ch;
                    for (int c = 0; c < ch; c++) fr[c] = m;
                }
                for (int c = 0; c < ch; c++) *o++ = fr[c] * g;
            }
            else
            {
                for (int c = 0; c < ch; c++) *o++ = src[c] * g;
            }
            pos++;
        }
        else
        {
            for (int c = 0; c < ch; c++) *o++ = 0.0f;
            p->reachedEnd_.store(true);
        }
    }
    p->pos_.store(pos);
}

void Player::rebuildMonitorLocked()
{
    monFilters_.clear();
    if (devSr_ <= 0) return;
    for (const auto& s : monSpecs_)
        monFilters_.push_back(Bq::make(s, devSr_));
}

void Player::setMonitor(const std::vector<Bq::Spec>& specs, bool mono)
{
    std::lock_guard<std::mutex> lk(monM_);
    monSpecs_ = specs;
    monMono_ = mono;
    rebuildMonitorLocked();
}

void Player::ensureDevice(int channels, int sampleRate)
{
    if (deviceInit_ && devCh_ == channels && devSr_ == sampleRate) return;
    if (deviceInit_) { ma_device_uninit(&device_); deviceInit_ = false; }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate = (ma_uint32)sampleRate;
    cfg.dataCallback = &Player::dataCb;
    cfg.pUserData = this;

    if (ma_device_init(NULL, &cfg, &device_) == MA_SUCCESS)
    {
        deviceInit_ = true;
        devCh_ = channels;
        devSr_ = sampleRate;
        std::lock_guard<std::mutex> lk(monM_);
        rebuildMonitorLocked();   // 新しいサンプルレートで係数を再計算
    }
}

void Player::play(const AudioClip& clip, long long s, long long e, long long loopStart)
{
    stop();
    ensureDevice(clip.channels, clip.sampleRate);
    if (!deviceInit_) return;

    samples_ = clip.samples.data();
    channels_ = clip.channels;
    start_ = (uint64_t)s;
    loopStart_ = (uint64_t)(loopStart >= 0 ? loopStart : s);
    end_ = (uint64_t)e;
    pos_.store((uint64_t)s);
    reachedEnd_.store(false);
    state_ = State::Playing;
    ma_device_start(&device_);
}

void Player::pause()
{
    if (state_ == State::Playing && deviceInit_)
    {
        ma_device_stop(&device_);
        state_ = State::Paused;
    }
}

void Player::resume()
{
    if (state_ == State::Paused && deviceInit_)
    {
        ma_device_start(&device_);
        state_ = State::Playing;
    }
}

void Player::stop()
{
    if (deviceInit_ && state_ != State::Stopped)
        ma_device_stop(&device_);
    state_ = State::Stopped;
}

void Player::update()
{
    if (state_ == State::Playing && reachedEnd_.load())
        stop();
}
