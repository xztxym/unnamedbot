#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace UnnamedBot {

    // 极简 PCM16 播放器（WASAPI 共享模式）。
    //
    // 设计目的只有一件事：把在线 TTS 拿回来的 16bit PCM 播出声音，并顺手给出一个
    // 0..1 的电平包络，供 Live2D 嘴型使用。不做混音、不做多路、不做重采样质量优化
    // （线性插值足够语音用），也不持有任何第三方依赖 —— 只用系统 WASAPI。
    class AudioPlayer {
    public:
        static AudioPlayer& Instance();

        // pcm 交叠存放（mono 或 stereo）。播放是异步的：入队即返回。
        bool PlayPcm16(const std::vector<int16_t>& samples, int sampleRate, int channels);

        // 清空队列并立刻停止当前播放（打断用）
        void Stop();

        void Shutdown();

        // 主循环每帧调用：没有声音在播时让电平平滑回落（release），
        // 避免嘴最后卡在张开的姿态上。
        void Tick();

        // 0..1 的电平（已做 attack/release 平滑）
        float Level() const { return level_.load(std::memory_order_relaxed); }

        bool IsPlaying() const { return playing_.load(std::memory_order_relaxed); }

        // 嘴型增益（配置 voice.lip_sync_gain）
        void SetGain(float gain) { gain_ = gain > 0.0f ? gain : 1.0f; }

    private:
        struct Buffer {
            std::vector<int16_t> samples;   // 交叠 PCM16
            int sampleRate = 24000;
            int channels = 1;
        };

        AudioPlayer() = default;
        ~AudioPlayer();
        AudioPlayer(const AudioPlayer&) = delete;
        AudioPlayer& operator=(const AudioPlayer&) = delete;

        void EnsureWorker();
        void WorkerLoop();

        // 播放一整块（阻塞到这块播完或被打断）
        void PlayOne(const Buffer& buffer);

        void PushLevel(float rms);

        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Buffer> queue_;
        std::atomic<bool> running_{ false };
        std::atomic<bool> stopping_{ false };
        std::atomic<bool> playing_{ false };
        std::atomic<float> level_{ 0.0f };
        float gain_ = 1.6f;
    };

} // namespace UnnamedBot
