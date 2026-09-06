#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

    // 消息角色（与AIClient保持一致）
    enum class MessageRole {
        System,
        User,
        Assistant,
        Tool
    };

    // 单条对话消息（扩展版）
    struct ConversationMessage {
        int64_t id = 0;
        int64_t sessionId = 0;
        MessageRole role = MessageRole::User;
        std::string content;
        int64_t timestamp = 0;
        int tokenCount = 0;
        nlohmann::json extraData;           // 附件、工具调用等
        std::string triggerSource;          // 触发来源（主动触发时使用）
        bool isActiveTrigger = false;       // 是否是主动触发消息
    };

    // 会话信息
    struct Session {
        int64_t id = 0;
        std::string name;
        int64_t createdAt = 0;
        int64_t updatedAt = 0;
        bool isActive = true;
        nlohmann::json modelConfig;         // 采样参数、模型配置
        std::string systemPrompt;
        std::string prePrompt;
        std::string postPrompt;
        std::vector<ConversationMessage> messages;
    };

    // 主动触发事件类型
    enum class TriggerEventType {
        None,
        ProcessChange,      // 前台进程变化
        WeatherUpdate,      // 天气更新
        IPChange,           // IP位置变化
        ScreenCapture,      // 屏幕截图
        Timer,              // 定时触发
        Custom              // 自定义事件
    };

    // 主动触发事件
    struct TriggerEvent {
        TriggerEventType type = TriggerEventType::None;
        std::string description;
        nlohmann::json data;
        int64_t timestamp = 0;
    };

    // 消息处理器回调
    using NewMessageCallback = std::function<void(const ConversationMessage& message)>;
    using TriggerCallback = std::function<void(const TriggerEvent& event)>;
    using AIRequestCallback = std::function<std::string(
        const std::vector<ConversationMessage>& messages,
        std::vector<ConversationMessage>& aiResponse
    )>;

    // 消息处理器
    class MessageHandler {
    public:
        static MessageHandler& Instance();

        // 初始化
        void Initialize();

        // 关闭
        void Shutdown();

        // ============ 会话管理 ============

        // 创建新会话
        int64_t CreateSession(const std::string& name = "New Session");

        // 切换会话
        bool SwitchSession(int64_t sessionId);

        // 获取当前会话
        std::shared_ptr<Session> GetCurrentSession() const;

        // 获取所有会话
        std::vector<Session> GetAllSessions() const;

        // 删除会话
        bool DeleteSession(int64_t sessionId);

        // 重命名会话
        bool RenameSession(int64_t sessionId, const std::string& newName);

        // 清空会话消息
        bool ClearSessionMessages(int64_t sessionId);

        // ============ 消息管理 ============

        // 添加消息到当前会话
        int64_t AddMessage(
            MessageRole role,
            const std::string& content,
            const nlohmann::json& extraData = {},
            const std::string& triggerSource = "",
            bool isActiveTrigger = false
        );

        // 添加AI回复消息（自动处理Token统计）
        int64_t AddAIResponse(
            const std::string& content,
            int promptTokens = 0,
            int completionTokens = 0,
            const nlohmann::json& extraData = {}
        );

        // 获取会话消息列表
        std::vector<ConversationMessage> GetSessionMessages(
            int64_t sessionId,
            int limit = 0,
            int offset = 0
        ) const;

        // 获取当前会话消息
        std::vector<ConversationMessage> GetCurrentMessages(int limit = 0) const;

        // 删除单条消息
        bool DeleteMessage(int64_t messageId);

        // 获取消息历史（用于AI请求）
        std::vector<ConversationMessage> GetHistoryForAI(int64_t sessionId) const;

        // ============ 上下文管理 ============

        // 裁剪历史（根据最大Token数或轮次数）
        std::vector<ConversationMessage> TrimHistory(
            const std::vector<ConversationMessage>& messages,
            int maxTokens = 4096,
            int maxRounds = 30
        ) const;

        // 构建AI请求消息（包含系统提示、前后缀）
        std::vector<Message> BuildAIMessages(
            const std::vector<ConversationMessage>& messages,
            const std::string& systemPrompt = "",
            const std::string& prePrompt = "",
            const std::string& postPrompt = ""
        ) const;

        // ============ 主动触发管理 ============

        // 处理主动触发事件
        bool HandleTriggerEvent(const TriggerEvent& event);

        // 注册触发回调
        void SetTriggerCallback(TriggerCallback callback);

        // 设置AI请求回调（用于主动触发时调用AI）
        void SetAIRequestCallback(AIRequestCallback callback);

        // 检查冷却状态
        bool IsInCooldown() const;

        // 获取上次触发时间
        int64_t GetLastTriggerTime() const;

        // 重置冷却
        void ResetCooldown();

        // ============ 事件监听 ============

        // 注册新消息回调（用于UI更新）
        void OnNewMessage(NewMessageCallback callback);

        // ============ 搜索和导出 ============

        // 搜索消息
        std::vector<ConversationMessage> SearchMessages(
            int64_t sessionId,
            const std::string& keyword,
            int limit = 50
        ) const;

        // 导出会话到JSON
        bool ExportSession(int64_t sessionId, const std::wstring& filePath) const;

        // 导入会话从JSON
        bool ImportSession(const std::wstring& filePath);

        // ============ 配置 ============

        // 更新会话配置
        void UpdateSessionConfig(
            int64_t sessionId,
            const nlohmann::json& config
        );

        // 获取会话配置
        nlohmann::json GetSessionConfig(int64_t sessionId) const;

    private:
        MessageHandler() = default;
        ~MessageHandler() = default;
        MessageHandler(const MessageHandler&) = delete;
        MessageHandler& operator=(const MessageHandler&) = delete;

        // 保存消息到数据库
        bool SaveMessageToDB(const ConversationMessage& message);

        // 从数据库加载会话消息
        void LoadSessionMessages(int64_t sessionId);

        // 获取当前时间戳
        int64_t GetCurrentTimestamp() const;

        // 计算消息Token数（近似）
        int EstimateTokens(const std::string& text) const;

        // 更新会话更新时间
        void UpdateSessionTimestamp(int64_t sessionId);

        // 处理主动触发生成AI回复
        void ProcessTriggerWithAI(const TriggerEvent& event);

        // 当前会话
        std::shared_ptr<Session> currentSession_;
        int64_t currentSessionId_ = -1;

        // 所有会话缓存
        std::map<int64_t, std::shared_ptr<Session>> sessions_;

        // 冷却状态
        int64_t lastTriggerTime_ = 0;
        int64_t cooldownSeconds_ = 480; // 默认8分钟

        // 回调
        NewMessageCallback newMessageCallback_;
        TriggerCallback triggerCallback_;
        AIRequestCallback aiRequestCallback_;

        // 线程安全
        mutable std::mutex mutex_;

        // 初始化标志
        bool initialized_ = false;

        // 数据库缓存
        bool messagesLoaded_ = false;
    };

} // namespace UnnamedBot