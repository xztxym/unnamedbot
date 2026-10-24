#pragma once

#include <string>
#include <mutex>

namespace UnnamedBot {

    class AIClient;

    // 朗读前翻译：本地英文音色只认英文，而 AI 回复默认是中文。
    // 这里用用户已配置的对话模型做一次「只输出译文」的最小请求，
    // 目标语言固定为英文（Kokoro 只有 en 前端可用）。
    //
    // 设计约束：
    //   - 绝不能因为翻译失败让朗读静默卡死：失败返回 false，调用方可以选择跳过这一句；
    //   - 不做长文本总结/改写，只做直译，保持与屏幕文字的语义同步；
    //   - 结果去掉模型可能加上的引号与「Translation:」前缀。
    class VoiceTranslator {
    public:
        static VoiceTranslator& Instance();

        // 宿主启动时把 AppCore 持有的 AIClient 挂进来（语音线程里用它做翻译）
        void SetClient(AIClient* client);

        // 文本里是否含中日韩字符（英文/数字/标点直接念，不用翻）
        static bool NeedsTranslation(const std::string& text);

        // 翻译成英文；失败时 error 带原因
        bool Translate(const std::string& text, std::string& out, std::string& error);

    private:
        AIClient* client_ = nullptr;
        std::mutex mutex_;
        VoiceTranslator() = default;
        ~VoiceTranslator() = default;
        VoiceTranslator(const VoiceTranslator&) = delete;
        VoiceTranslator& operator=(const VoiceTranslator&) = delete;
    };

} // namespace UnnamedBot
