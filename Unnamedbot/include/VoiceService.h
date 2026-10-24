#pragma once

#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

namespace UnnamedBot {

    // 语音服务：把「一句 AI 回复」变成「声音 + 嘴型电平」。
    //
    // P0 只做联网后端（tts.quest 的 VOICEVOX 兼容接口），因此：
    //   - 不需要任何本地模型 / 引擎，安装体积不增加；
    //   - 拿到的是 24kHz/16bit WAV，直接交给 AudioPlayer；
    //   - 情绪通过 VOICEVOX 的 styleId 表达（ずんだもん あまあま / ツンツン / ささやき …）。
    // 失败一律静默（只写一条日志），不能因为断网把聊天流程卡住。
    class VoiceService {
    public:
        static VoiceService& Instance();

        void Initialize();
        void Shutdown();

        // 配置是否允许出声（voice.enabled 且 backend 不是 off）
        bool Available() const { return available_.load(); }

        // 说一段话（内部会按句切分、排队）。emotion 见 EmotionStyleId。
        void Speak(const std::string& text, const std::string& emotion = "calm");

        // 设置页「试听」按钮：用 voice.test_text 合成一句
        void TestSpeak();

        void Stop();

        // 主循环每帧调用：驱动嘴型电平的回落
        void Tick();

        float Level() const;

        // 配置改动后重新读一遍（设置保存时调用）
        void ReloadConfig();

        // 设置页用的角色列表（Enum 选项的唯一样例来源）
        static std::vector<std::wstring> SpeakerLabels();

        // 角色 + 情绪 → VOICEVOX styleId
        static int EmotionStyleId(const std::wstring& speakerLabel, const std::string& emotion);

        // 把「ずんだもん」这类标签解析成 styleId（找不到时回退到默认角色）
        static int BaseStyleId(const std::wstring& speakerLabel);

    private:
        struct Job {
            std::string text;
            std::string emotion;
            int baseStyle = 3;
        };

        VoiceService() = default;
        ~VoiceService();
        VoiceService(const VoiceService&) = delete;
        VoiceService& operator=(const VoiceService&) = delete;

        void EnsureWorker();
        void WorkerLoop();
        bool SpeakBlocking(const Job& job);
        bool SynthesizeOnline(const std::string& sentence, int styleId,
            std::vector<int16_t>& pcm, int& sampleRate, int& channels, std::string& error);
        std::vector<std::string> SplitSentences(const std::string& text) const;

        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Job> queue_;
        std::atomic<bool> running_{ false };
        std::atomic<bool> available_{ false };
        std::atomic<bool> stopRequested_{ false };

        // 配置快照（ReloadConfig 时整体替换，避免每句都查库）
        std::atomic<bool> enabled_{ false };
        std::string backend_ = "ttsquest";
        bool localBackend_ = false;      // 本地 Kokoro（英文）
        bool translate_ = true;          // 朗读前把中文翻成英文（本地音色只认英文）
        std::string localVoice_ = "af_heart";
        std::wstring speaker_ = L"ずんだもん";
        int baseStyle_ = 3;
        double speed_ = 1.0;
        double pitch_ = 0.0;
        double intonation_ = 1.0;
        double volume_ = 1.0;
        std::string speakMode_ = "auto";
        std::string testText_;
        int maxChars_ = 120;
        bool lipSync_ = true;
        float lipGain_ = 1.6f;
    };

} // namespace UnnamedBot
