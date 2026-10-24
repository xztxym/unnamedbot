#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace UnnamedBot {

    // 本地英文语音后端：Kokoro-82M（ONNX，量化 88MB）+ espeak-ng 音素前端。
    //
    // 设计取舍：
    //   - onnxruntime.dll / espeak-ng.dll 全部用 LoadLibraryW 动态加载，
    //     这样不用在工程里挂 .lib，引擎目录可以整体外置（runtime\voice_engine\kokoro）。
    //   - 只做英文：espeak-ng 的 ja / cmn 前端质量不行（kanji 乱读、丢声调），
    //     中文与日语继续走联网后端。
    //   - 全程无全局状态泄漏：Uninitialize 释放会话与 DLL。
    class KokoroTts {
    public:
        static KokoroTts& Instance();

        // engineDir: 含 model.onnx / onnxruntime.dll / espeak-ng.dll / espeak-ng-data / voices
        bool Initialize(const std::wstring& engineDir);
        void Shutdown();
        bool Available() const { return available_; }

        // 音色名（voices\<name>.bin，裸 float32 [rows,1,256]）
        std::vector<std::string> Voices() const;
        bool HasVoice(const std::string& voice) const;

        // text: 纯英文（可含标点）；speed: 0.5~2.0
        // 输出 24kHz / mono / PCM16
        bool Synthesize(const std::string& text, const std::string& voice, float speed,
            std::vector<int16_t>& pcm, int& sampleRate, std::string& error);

    private:
        KokoroTts() = default;
        ~KokoroTts();
        KokoroTts(const KokoroTts&) = delete;
        KokoroTts& operator=(const KokoroTts&) = delete;

        bool LoadVoiceRows(const std::string& voice, std::vector<float>& rows, int& count);
        bool Phonemize(const std::string& text, std::string& phonemes, std::string& error);

        mutable std::mutex mutex_;
        bool available_ = false;
        std::wstring engineDir_;
        std::string modelPath_;
        std::string voicesDir_;
        void* ortDll_ = nullptr;      // HMODULE
        void* espeakDll_ = nullptr;   // HMODULE
        void* api_ = nullptr;         // const OrtApi*
        void* env_ = nullptr;         // OrtEnv*
        void* session_ = nullptr;     // OrtSession*
        void* memoryInfo_ = nullptr;  // OrtMemoryInfo*
        std::string currentVoice_;
        std::vector<float> currentRows_;
        int currentRowCount_ = 0;
    };

} // namespace UnnamedBot
