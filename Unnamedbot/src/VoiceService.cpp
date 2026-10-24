#include "../include/VoiceService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "../include/AudioPlayer.h"
#include "../include/KokoroTts.h"
#include "../include/VoiceTranslator.h"
#include "../include/ConfigManager.h"
#include "../include/Logger.h"
#include "../include/Utils.h"

namespace UnnamedBot {

    namespace {

        const char* const kSynthesisUrl = "https://api.tts.quest/v3/voicevox/synthesis";

        // 角色表：标签（设置页显示）→ VOICEVOX 默认 styleId
        struct SpeakerDef {
            const wchar_t* label;
            int baseStyle;
        };

        const SpeakerDef kSpeakers[] = {
            { L"ずんだもん",   3 },
            { L"四国めたん",   2 },
            { L"春日部つむぎ", 8 },
            { L"冥鳴ひまり",  14 },
            { L"もち子さん",  20 },
            { L"波音リツ",     9 },
            { L"雨晴はう",    10 },
        };
        const int kSpeakerCount = static_cast<int>(sizeof(kSpeakers) / sizeof(kSpeakers[0]));

        // 本地 Kokoro（英文）音色：标签 → voices\<file>.bin
        struct LocalVoiceDef {
            const wchar_t* label;
            const char* file;
        };

        const LocalVoiceDef kLocalVoices[] = {
            { L"Kokoro · af_heart（女）",   "af_heart" },
            { L"Kokoro · af_bella（女）",   "af_bella" },
            { L"Kokoro · af_nicole（女）",  "af_nicole" },
            { L"Kokoro · am_michael（男）", "am_michael" },
        };
        const int kLocalVoiceCount = static_cast<int>(sizeof(kLocalVoices) / sizeof(kLocalVoices[0]));

        bool IsLocalBackend(const std::string& backend) {
            return backend.find("Kokoro") != std::string::npos || backend == "本地 Kokoro（英文）";
        }

        std::string LocalVoiceFile(const std::wstring& label) {
            for (int i = 0; i < kLocalVoiceCount; ++i) {
                if (label == kLocalVoices[i].label) return kLocalVoices[i].file;
            }
            return "af_heart";
        }

        // 情绪 → styleId。VOICEVOX 的情绪就是「风格（style）」：
        // ずんだもん: 3 ノーマル / 1 あまあま / 7 ツンツン / 23 ささやき / 25 ヒソヒソ
        // 四国めたん: 2 / 0 / 6 / 22 / 24
        int EmotionStyle(int baseStyle, const std::string& emotion) {
            if (baseStyle == 3) {           // ずんだもん
                if (emotion == "happy") return 1;
                if (emotion == "angry") return 7;
                if (emotion == "sad") return 23;
                if (emotion == "whisper") return 23;
                if (emotion == "shy") return 25;
                return 3;
            }
            if (baseStyle == 2) {           // 四国めたん
                if (emotion == "happy") return 0;
                if (emotion == "angry") return 6;
                if (emotion == "sad") return 22;
                if (emotion == "whisper") return 22;
                if (emotion == "shy") return 24;
                return 2;
            }
            return baseStyle;               // 其他角色只有默认风格
        }

        // WAV 解析：从 RIFF 里取 fmt / data 两块，转成 int16 交叠
        bool ParseWav(const std::string& bytes, std::vector<int16_t>& pcm, int& sampleRate, int& channels) {
            if (bytes.size() < 44 ||
                std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
                std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
                return false;
            }

            size_t pos = 12;
            int bitsPerSample = 16;
            size_t dataOffset = 0;
            size_t dataSize = 0;
            sampleRate = 0;
            channels = 0;

            while (pos + 8 <= bytes.size()) {
                const char* id = bytes.data() + pos;
                uint32_t size = 0;
                std::memcpy(&size, bytes.data() + pos + 4, 4);
                const size_t chunkStart = pos + 8;

                if (std::memcmp(id, "fmt ", 4) == 0 && chunkStart + 16 <= bytes.size()) {
                    uint16_t ch = 0, bps = 0;
                    uint32_t rate = 0;
                    std::memcpy(&ch, bytes.data() + chunkStart + 2, 2);
                    std::memcpy(&rate, bytes.data() + chunkStart + 4, 4);
                    std::memcpy(&bps, bytes.data() + chunkStart + 14, 2);
                    channels = static_cast<int>(ch);
                    sampleRate = static_cast<int>(rate);
                    bitsPerSample = static_cast<int>(bps);
                } else if (std::memcmp(id, "data", 4) == 0) {
                    dataOffset = chunkStart;
                    dataSize = std::min<size_t>(size, bytes.size() - chunkStart);
                    break;
                }

                pos = chunkStart + size + (size & 1u);
            }

            if (channels <= 0 || sampleRate <= 0 || dataOffset == 0 || dataSize == 0) {
                return false;
            }
            if (bitsPerSample != 16) {
                return false;   // tts.quest 固定 16bit，够用
            }

            const size_t samples = dataSize / sizeof(int16_t);
            pcm.resize(samples);
            std::memcpy(pcm.data(), bytes.data() + dataOffset, samples * sizeof(int16_t));
            return true;
        }

        std::string ReadString(const char* key, const std::string& fallback) {
            return ConfigManager::Instance().GetString(key, fallback);
        }

    } // namespace

    VoiceService& VoiceService::Instance() {
        static VoiceService instance;
        return instance;
    }

    VoiceService::~VoiceService() {
        Shutdown();
    }

    std::vector<std::wstring> VoiceService::SpeakerLabels() {
        std::vector<std::wstring> labels;
        labels.reserve(kSpeakerCount + kLocalVoiceCount);
        for (int i = 0; i < kLocalVoiceCount; ++i) {
            labels.emplace_back(kLocalVoices[i].label);   // 本地英文音色排最前，选中概率高
        }
        for (int i = 0; i < kSpeakerCount; ++i) {
            labels.emplace_back(kSpeakers[i].label);
        }
        return labels;
    }

    int VoiceService::BaseStyleId(const std::wstring& speakerLabel) {
        for (int i = 0; i < kSpeakerCount; ++i) {
            if (speakerLabel == kSpeakers[i].label) {
                return kSpeakers[i].baseStyle;
            }
        }
        return 3;   // 默认ずんだもん
    }

    int VoiceService::EmotionStyleId(const std::wstring& speakerLabel, const std::string& emotion) {
        return EmotionStyle(BaseStyleId(speakerLabel), emotion);
    }

    void VoiceService::Initialize() {
        ReloadConfig();
        AudioPlayer::Instance().SetGain(lipGain_);
        EnsureWorker();
        LOG_INFO("Voice", available_.load()
            ? ("Voice service ready (backend=" + backend_ + ", lipSync=" + (lipSync_ ? "on" : "off") + ").")
            : std::string("Voice service disabled."));

        // 诊断用自检：voice.selftest=true 时启动即合成一句，日志里能看到链路是否通
        if (available_.load() && ConfigManager::Instance().GetBool("voice.selftest", false)) {
            LOG_INFO("Voice", "Self-test enabled (voice.selftest=true), speaking test text.");
            TestSpeak();
        }
    }

    void VoiceService::ReloadConfig() {
        enabled_ = ConfigManager::Instance().GetBool("voice.enabled", false);
        backend_ = ReadString("voice.backend", "ttsquest");

        const std::wstring label = Utils::Utf8ToWide(ReadString("voice.speaker", "ずんだもん"));
        speaker_ = label.empty() ? std::wstring(L"ずんだもん") : label;
        baseStyle_ = BaseStyleId(speaker_);

        speed_ = ConfigManager::Instance().GetDouble("voice.speed", 1.0);
        pitch_ = ConfigManager::Instance().GetDouble("voice.pitch", 0.0);
        intonation_ = ConfigManager::Instance().GetDouble("voice.intonation", 1.0);
        volume_ = ConfigManager::Instance().GetDouble("voice.volume", 1.0);

        speakMode_ = ReadString("voice.speak_mode", "自动");
        testText_ = ReadString("voice.test_text", "你好，我是桌宠。");
        if (testText_.empty()) {
            testText_ = "你好，我是桌宠。";
        }
        maxChars_ = std::clamp(ConfigManager::Instance().GetInt("voice.max_chars", 120), 20, 400);
        translate_ = ConfigManager::Instance().GetBool("voice.translate", true);
        lipSync_ = ConfigManager::Instance().GetBool("voice.lip_sync", true);
        lipGain_ = static_cast<float>(std::clamp(ConfigManager::Instance().GetDouble("voice.lip_sync_gain", 1.6), 0.2, 4.0));

        AudioPlayer::Instance().SetGain(lipGain_);

        // 设置页里存的是中文值（在线 VOICEVOX / ElevenLabs / 本地 Kokoro（英文）/ 关闭），旧配置的英文值也兼容
        localBackend_ = IsLocalBackend(backend_);
        if (localBackend_) {
            const std::wstring engineDir = Utils::ResolveAppRelative(L"voice_engine\\kokoro");
            if (!KokoroTts::Instance().Initialize(engineDir)) {
                LOG_WARN("Voice", "Local Kokoro engine unavailable, falling back to online backend.");
                localBackend_ = false;
                backend_ = "在线 VOICEVOX";
            } else {
                localVoice_ = LocalVoiceFile(speaker_);
            }
        }
        const bool backendOk = (backend_ == "在线 VOICEVOX" || backend_ == "ttsquest" ||
            backend_ == "ElevenLabs" || backend_ == "elevenlabs" || localBackend_);
        available_.store(enabled_.load() && backendOk && speakMode_ != "关闭");
    }

    void VoiceService::EnsureWorker() {
        if (running_.load()) {
            return;
        }
        running_.store(true);
        worker_ = std::thread(&VoiceService::WorkerLoop, this);
    }

    void VoiceService::Shutdown() {
        if (!running_.exchange(false)) {
            return;
        }
        stopRequested_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        AudioPlayer::Instance().Shutdown();
    }

    void VoiceService::Stop() {
        stopRequested_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        AudioPlayer::Instance().Stop();
    }

    float VoiceService::Level() const {
        return lipSync_ ? AudioPlayer::Instance().Level() : 0.0f;
    }

    void VoiceService::Tick() {
        AudioPlayer::Instance().Tick();
    }

    std::vector<std::string> VoiceService::SplitSentences(const std::string& text) const {
        std::vector<std::string> out;
        std::string current;
        const size_t maxBytes = static_cast<size_t>(maxChars_) * 3;   // 中文按 3 字节估

        for (size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            size_t len = 1;
            if ((c & 0x80) == 0) len = 1;
            else if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;

            current.append(text, i, len);
            i += len;

            const bool isBreak = current.size() >= 3 &&
                (current.compare(current.size() - 3, 3, "。") == 0 ||
                    current.compare(current.size() - 3, 3, "！") == 0 ||
                    current.compare(current.size() - 3, 3, "？") == 0);
            const bool isAsciiBreak = (len == 1 && (c == '.' || c == '!' || c == '?' || c == '\n'));

            if (isBreak || isAsciiBreak || current.size() >= maxBytes) {
                // 去掉动作/括号标记（如（小声））与首尾空白，避免念出来
                std::string sentence = current;
                const size_t open = sentence.find('（');
                const size_t close = sentence.find('）');
                if (open != std::string::npos && close != std::string::npos && close > open) {
                    sentence.erase(open, close - open + 3);
                }
                if (!sentence.empty() && sentence.back() == '\n') {
                    sentence.pop_back();
                }
                if (!sentence.empty()) {
                    out.push_back(sentence);
                }
                current.clear();
            }
        }

        if (!current.empty()) {
            out.push_back(current);
        }
        return out;
    }

    void VoiceService::Speak(const std::string& text, const std::string& emotion) {
        if (!available_.load() || text.empty()) {
            return;
        }

        // speak_mode：自动 = 每句都念；仅标签 = 只有带 <voice> 标记的才念
        if (speakMode_ == "仅标签" && emotion.empty()) {
            return;
        }

        EnsureWorker();

        const std::vector<std::string> sentences = SplitSentences(text);
        if (sentences.empty()) {
            return;
        }

        const int styleId = EmotionStyle(baseStyle_, emotion.empty() ? "calm" : emotion);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& s : sentences) {
                queue_.push_back(Job{ s, emotion, styleId });
            }
        }
        stopRequested_.store(false);
        cv_.notify_one();
        LOG_INFO("Voice", "Queued " + std::to_string(sentences.size()) + " sentence(s), styleId=" +
            std::to_string(styleId) + ".");
    }

    void VoiceService::TestSpeak() {
        if (!available_.load()) {
            LOG_WARN("Voice", "Test skipped: voice is disabled.");
            return;
        }
        // 试听也要按句切：这样听到的就是真实回复的节奏（逐句翻译 + 逐句合成）
        const std::vector<std::string> sentences = SplitSentences(testText_);
        EnsureWorker();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sentences.empty()) {
                queue_.push_back(Job{ testText_, "calm", baseStyle_ });
            } else {
                for (const auto& s : sentences) {
                    queue_.push_back(Job{ s, "calm", baseStyle_ });
                }
            }
        }
        stopRequested_.store(false);
        cv_.notify_one();
        LOG_INFO("Voice", "Test speak: " + testText_ + " (styleId=" + std::to_string(baseStyle_) + ")");
    }

    void VoiceService::WorkerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
                if (!running_.load()) {
                    break;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }

            if (stopRequested_.load()) {
                continue;
            }
            SpeakBlocking(job);
        }
    }

    bool VoiceService::SpeakBlocking(const Job& job) {
        std::vector<int16_t> pcm;
        int sampleRate = 0;
        int channels = 0;
        std::string error;

        // 本地音色只认英文：中文回复先过一次翻译，失败就跳过这句（宁可不出声也不念错）
        std::string speakText = job.text;
        if (localBackend_ && translate_ && VoiceTranslator::NeedsTranslation(speakText)) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string english;
            std::string terr;
            if (!VoiceTranslator::Instance().Translate(speakText, english, terr)) {
                LOG_WARN("Voice", "Translate failed, sentence skipped: " + terr);
                return false;
            }
            const auto tms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Voice", "Translated in " + std::to_string(tms) + " ms: " + english);
            speakText = english;
        }

        const auto started = std::chrono::steady_clock::now();
        if (localBackend_) {
            channels = 1;
            if (!KokoroTts::Instance().Synthesize(speakText, localVoice_,
                static_cast<float>(speed_), pcm, sampleRate, error)) {
                LOG_WARN("Voice", "Local synthesis failed: " + error);
                return false;
            }
        } else if (!SynthesizeOnline(job.text, job.baseStyle, pcm, sampleRate, channels, error)) {
            LOG_WARN("Voice", "Synthesis failed: " + error);
            return false;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        LOG_INFO("Voice", "Synthesis ok in " + std::to_string(elapsed) + " ms, " +
            std::to_string(pcm.size() / (channels > 0 ? channels : 1)) + " frames @ " +
            std::to_string(sampleRate) + " Hz.");

        AudioPlayer::Instance().PlayPcm16(pcm, sampleRate, channels);
        return true;
    }

    bool VoiceService::SynthesizeOnline(const std::string& sentence, int styleId,
        std::vector<int16_t>& pcm, int& sampleRate, int& channels, std::string& error) {

        // 1) 提交合成请求
        cpr::Response resp = cpr::Get(
            cpr::Url{ kSynthesisUrl },
            cpr::Parameters{
                { "text", sentence },
                { "speaker", std::to_string(styleId) },
                { "speedScale", std::to_string(speed_) },
                { "pitchScale", std::to_string(pitch_) },
                { "intonationScale", std::to_string(intonation_) },
                { "volumeScale", std::to_string(volume_) },
            },
            cpr::Timeout{ 20000 });

        if (resp.status_code != 200) {
            error = "http " + std::to_string(resp.status_code);
            return false;
        }

        nlohmann::json json;
        try {
            json = nlohmann::json::parse(resp.text);
        } catch (const std::exception& e) {
            error = std::string("bad json: ") + e.what();
            return false;
        }

        if (!json.value("success", false)) {
            error = "api rejected";
            return false;
        }

        const std::string statusUrl = json.value("audioStatusUrl", "");
        const std::string wavUrl = json.value("wavDownloadUrl", "");
        if (statusUrl.empty() || wavUrl.empty()) {
            error = "missing urls";
            return false;
        }

        // 2) 轮询状态（服务端异步合成，通常 <1s 完成）
        bool ready = false;
        for (int i = 0; i < 25 && !stopRequested_.load(); ++i) {
            cpr::Response st = cpr::Get(cpr::Url{ statusUrl }, cpr::Timeout{ 8000 });
            if (st.status_code == 200) {
                try {
                    const auto s = nlohmann::json::parse(st.text);
                    const std::string status = s.value("status", "");
                    if (status == "done") {
                        ready = true;
                        break;
                    }
                    if (status == "failed") {
                        error = "service reports failed";
                        return false;
                    }
                } catch (...) {
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!ready) {
            error = "status timeout";
            return false;
        }

        // 3) 下载 wav
        cpr::Response wav = cpr::Get(cpr::Url{ wavUrl }, cpr::Timeout{ 30000 });
        if (wav.status_code != 200 || wav.text.size() < 44) {
            error = "wav download failed";
            return false;
        }

        if (!ParseWav(wav.text, pcm, sampleRate, channels)) {
            error = "wav parse failed";
            return false;
        }
        return true;
    }

} // namespace UnnamedBot
