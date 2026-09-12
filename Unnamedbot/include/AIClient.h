#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

namespace UnnamedBot {

    // 消息角色
    enum class MessageRole {
        System,
        User,
        Assistant,
        Tool
    };

    // 单条消息
    struct Message {
        MessageRole role;
        std::string content;
        std::string name;           // Tool名称（用于tool角色）
        std::string toolCallId;     // Tool调用ID
        std::vector<nlohmann::json> attachments; // 附件信息（图片等）
        std::map<std::string, std::string> metadata;
    };

    // 工具定义（符合OpenAI Function Calling规范）
    struct ToolDefinition {
        std::string type = "function";
        struct Function {
            std::string name;
            std::string description;
            nlohmann::json parameters; // JSON Schema
        } function;
    };

    // 工具调用请求
    struct ToolCall {
        std::string id;
        std::string type;
        struct Function {
            std::string name;
            std::string arguments; // JSON字符串
        } function;
    };

    // 工具调用结果
    struct ToolResult {
        std::string toolCallId;
        std::string result; // JSON字符串
        bool success = true;
        std::string error;
    };

    // AI响应
    struct AIResponse {
        std::string content;                    // 最终文本内容
        std::vector<ToolCall> toolCalls;        // 工具调用列表
        std::string model;
        int promptTokens = 0;
        int completionTokens = 0;
        int totalTokens = 0;
        bool isStreaming = false;
        std::string finishReason;
        std::string rawResponse;
    };

    // 流式响应回调
    using StreamCallback = std::function<void(const std::string& chunk, bool isComplete)>;
    using ToolCallCallback = std::function<void(const ToolCall& toolCall)>;

    // AI客户端配置
    struct AIClientConfig {
        std::string baseUrl = "https://api.openai.com/v1";
        std::string apiKey;
        std::string organization;
        std::string model = "gpt-4o";
        double temperature = 1.0;
        double topP = 1.0;
        double frequencyPenalty = 0.0;
        double presencePenalty = 0.0;
        bool stream = true;
        int timeoutSeconds = 60;
        int maxRetries = 3;
        int maxTokens = 4096;
        std::string proxyType = "none"; // "none", "http", "socks5"
        std::string proxyHost;
        int proxyPort = 0;
    };

    // AI客户端
    class AIClient {
    public:
        AIClient();
        ~AIClient();

        // 设置配置
        void SetConfig(const AIClientConfig& config);

        // 获取配置
        const AIClientConfig& GetConfig() const { return config_; }

        // 发送聊天请求（非流式）
        AIResponse SendChatMessage(
            const std::vector<Message>& messages,
            const std::vector<ToolDefinition>& tools = {},
            const std::string& toolChoice = "auto"
        );

        // 发送聊天请求（流式）
        void SendChatMessageStream(
            const std::vector<Message>& messages,
            StreamCallback onChunk,
            ToolCallCallback onToolCall = nullptr,
            const std::vector<ToolDefinition>& tools = {},
            const std::string& toolChoice = "auto"
        );

        // 提交工具调用结果（用于多轮对话）
        AIResponse SubmitToolResults(
            const std::vector<Message>& messages,
            const std::vector<ToolResult>& toolResults,
            const std::vector<ToolDefinition>& tools = {}
        );

        // 测试连接（验证API Key是否有效）
        bool TestConnection(std::string& errorMessage);

        // 获取可用模型列表
        std::vector<std::string> GetAvailableModels();

        // 设置代理
        void SetProxy(const std::string& type, const std::string& host, int port);

    private:
        // 构建请求体
        nlohmann::json BuildRequestBody(
            const std::vector<Message>& messages,
            const std::vector<ToolDefinition>& tools,
            const std::string& toolChoice,
            bool stream
        );

        // 解析响应（非流式）
        AIResponse ParseResponse(const std::string& responseBody);

        // 解析流式响应
        void ParseStreamResponse(
            const std::string& responseBody,
            StreamCallback onChunk,
            ToolCallCallback onToolCall
        );

        // 发送HTTP请求（带重试）
        std::string SendRequestWithRetry(
            const std::string& url,
            const nlohmann::json& body,
            int retryCount = 0
        );

        // 发送流式请求
        void SendStreamRequest(
            const std::string& url,
            const nlohmann::json& body,
            StreamCallback onChunk,
            ToolCallCallback onToolCall
        );

        // 消息转换为OpenAI格式
        nlohmann::json MessageToOpenAI(const Message& msg);

        // 工具定义转换为OpenAI格式
        nlohmann::json ToolToOpenAI(const ToolDefinition& tool);

        // 获取代理配置
        cpr::Proxies GetProxies() const;

        // 配置
        AIClientConfig config_;

        // 会话ID（用于关联请求）
        std::string sessionId_;

        // 请求计数器
        int requestCounter_ = 0;

        // 互斥锁
        mutable std::mutex mutex_;
    };

} // namespace UnnamedBot