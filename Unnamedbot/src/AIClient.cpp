#include "../include/AIClient.h"
#include "../include/Logger.h"
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <regex>

namespace UnnamedBot {

    AIClient::AIClient() {
        // 生成会话ID
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        sessionId_ = "session_" + std::to_string(timestamp);
    }

    AIClient::~AIClient() {
        // 清理资源
    }

    void AIClient::SetConfig(const AIClientConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        LOG_DEBUG("AIClient", "Config updated: BaseURL=" + config_.baseUrl +
            ", Model=" + config_.model +
            ", Stream=" + (config_.stream ? "true" : "false"));
    }

    AIResponse AIClient::SendChatMessage(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& toolChoice
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (config_.apiKey.empty()) {
            LOG_ERROR("AIClient", "API Key is empty.");
            AIResponse response;
            response.content = "Error: API Key not configured.";
            return response;
        }

        // 构建请求
        std::string url = config_.baseUrl + "/chat/completions";
        nlohmann::json body = BuildRequestBody(messages, tools, toolChoice, false);

        LOG_DEBUG("AIClient", "Sending request to: " + url + ", model: " + config_.model);

        // 发送请求
        std::string responseBody = SendRequestWithRetry(url, body);

        if (responseBody.empty()) {
            LOG_ERROR("AIClient", "Received empty response.");
            AIResponse response;
            response.content = "Error: Empty response from API.";
            return response;
        }

        // 解析响应
        return ParseResponse(responseBody);
    }

    void AIClient::SendChatMessageStream(
        const std::vector<Message>& messages,
        StreamCallback onChunk,
        ToolCallCallback onToolCall,
        const std::vector<ToolDefinition>& tools,
        const std::string& toolChoice
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (config_.apiKey.empty()) {
            LOG_ERROR("AIClient", "API Key is empty.");
            if (onChunk) {
                onChunk("Error: API Key not configured.", true);
            }
            return;
        }

        if (!onChunk) {
            LOG_ERROR("AIClient", "Stream callback is null.");
            return;
        }

        // 构建请求
        std::string url = config_.baseUrl + "/chat/completions";
        nlohmann::json body = BuildRequestBody(messages, tools, toolChoice, true);

        LOG_DEBUG("AIClient", "Sending stream request to: " + url);

        // 发送流式请求
        SendStreamRequest(url, body, onChunk, onToolCall);
    }

    AIResponse AIClient::SubmitToolResults(
        const std::vector<Message>& messages,
        const std::vector<ToolResult>& toolResults,
        const std::vector<ToolDefinition>& tools
    ) {
        // 构建包含工具结果的消息列表
        std::vector<Message> allMessages = messages;

        for (const auto& result : toolResults) {
            Message toolMsg;
            toolMsg.role = MessageRole::Tool;
            toolMsg.toolCallId = result.toolCallId;
            toolMsg.content = result.result;
            toolMsg.name = "unknown"; // 需要从上下文获取
            allMessages.push_back(toolMsg);
        }

        // 发送请求（非流式）
        return SendChatMessage(allMessages, tools);
    }

    bool AIClient::TestConnection(std::string& errorMessage) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            // 发送简单的模型列表请求来测试连接
            std::string url = config_.baseUrl + "/models";
            LOG_DEBUG("AIClient", "Testing connection to: " + url);

            cpr::Header headers = {
                {"Authorization", "Bearer " + config_.apiKey}
            };
            if (!config_.organization.empty()) {
                headers["OpenAI-Organization"] = config_.organization;
            }

            auto response = cpr::Get(
                cpr::Url{ url },
                headers,
                cpr::Timeout{ std::chrono::seconds(config_.timeoutSeconds) },
                cpr::Proxies{ GetProxies() }
            );

            if (response.status_code == 200) {
                LOG_INFO("AIClient", "Connection test successful.");
                errorMessage = "";
                return true;
            }
            else {
                errorMessage = "HTTP " + std::to_string(response.status_code) + ": " + response.text;
                LOG_ERROR("AIClient", "Connection test failed: " + errorMessage);
                return false;
            }
        }
        catch (const std::exception& e) {
            errorMessage = "Exception: " + std::string(e.what());
            LOG_ERROR("AIClient", "Connection test exception: " + errorMessage);
            return false;
        }
    }

    std::vector<std::string> AIClient::GetAvailableModels() {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> models;

        try {
            std::string url = config_.baseUrl + "/models";
            cpr::Header headers = {
                {"Authorization", "Bearer " + config_.apiKey}
            };
            if (!config_.organization.empty()) {
                headers["OpenAI-Organization"] = config_.organization;
            }

            auto response = cpr::Get(
                cpr::Url{ url },
                headers,
                cpr::Timeout{ std::chrono::seconds(config_.timeoutSeconds) },
                cpr::Proxies{ GetProxies() }
            );

            if (response.status_code == 200) {
                auto json = nlohmann::json::parse(response.text);
                if (json.contains("data") && json["data"].is_array()) {
                    for (const auto& item : json["data"]) {
                        if (item.contains("id") && item["id"].is_string()) {
                            models.push_back(item["id"]);
                        }
                    }
                }
                LOG_DEBUG("AIClient", "Retrieved " + std::to_string(models.size()) + " models.");
            }
            else {
                LOG_ERROR("AIClient", "Failed to get models: " + response.text);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("AIClient", "Exception getting models: " + std::string(e.what()));
        }

        // 如果无法获取，返回默认模型列表
        if (models.empty()) {
            models = { "gpt-4o", "gpt-4-turbo", "gpt-3.5-turbo" };
        }

        return models;
    }

    void AIClient::SetProxy(const std::string& type, const std::string& host, int port) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.proxyType = type;
        config_.proxyHost = host;
        config_.proxyPort = port;
        LOG_DEBUG("AIClient", "Proxy set: " + type + "://" + host + ":" + std::to_string(port));
    }

    // 私有方法实现

    nlohmann::json AIClient::BuildRequestBody(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& toolChoice,
        bool stream
    ) {
        nlohmann::json body;

        body["model"] = config_.model;
        body["temperature"] = config_.temperature;
        body["top_p"] = config_.topP;
        body["frequency_penalty"] = config_.frequencyPenalty;
        body["presence_penalty"] = config_.presencePenalty;
        body["max_tokens"] = config_.maxTokens;
        body["stream"] = stream;

        // 构建消息数组
        nlohmann::json msgArray = nlohmann::json::array();
        for (const auto& msg : messages) {
            msgArray.push_back(MessageToOpenAI(msg));
        }
        body["messages"] = msgArray;

        // 添加工具定义
        if (!tools.empty()) {
            nlohmann::json toolArray = nlohmann::json::array();
            for (const auto& tool : tools) {
                toolArray.push_back(ToolToOpenAI(tool));
            }
            body["tools"] = toolArray;

            if (!toolChoice.empty() && toolChoice != "auto") {
                if (toolChoice == "none") {
                    body["tool_choice"] = "none";
                }
                else {
                    body["tool_choice"] = nlohmann::json::object({
                        {"type", "function"},
                        {"function", {{"name", toolChoice}}}
                        });
                }
            }
        }

        return body;
    }

    AIResponse AIClient::ParseResponse(const std::string& responseBody) {
        AIResponse response;

        try {
            auto json = nlohmann::json::parse(responseBody);

            // 检查错误
            if (json.contains("error")) {
                std::string errorMsg = json["error"].dump();
                LOG_ERROR("AIClient", "API error: " + errorMsg);
                response.content = "API Error: " + errorMsg;
                return response;
            }

            // 提取基本信息
            if (json.contains("model")) {
                response.model = json["model"];
            }

            if (json.contains("usage")) {
                auto usage = json["usage"];
                if (usage.contains("prompt_tokens")) {
                    response.promptTokens = usage["prompt_tokens"];
                }
                if (usage.contains("completion_tokens")) {
                    response.completionTokens = usage["completion_tokens"];
                }
                if (usage.contains("total_tokens")) {
                    response.totalTokens = usage["total_tokens"];
                }
            }

            // 提取消息内容
            if (json.contains("choices") && json["choices"].is_array() && !json["choices"].empty()) {
                auto choice = json["choices"][0];
                auto message = choice["message"];

                // 提取finish_reason
                if (choice.contains("finish_reason")) {
                    response.finishReason = choice["finish_reason"];
                }

                // 提取内容
                if (message.contains("content") && !message["content"].is_null()) {
                    response.content = message["content"];
                }

                // 提取工具调用
                if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                    for (const auto& tc : message["tool_calls"]) {
                        ToolCall toolCall;
                        toolCall.id = tc["id"];
                        toolCall.type = tc["type"];
                        toolCall.function.name = tc["function"]["name"];
                        toolCall.function.arguments = tc["function"]["arguments"];
                        response.toolCalls.push_back(toolCall);
                    }
                }
            }

            response.rawResponse = responseBody;
        }
        catch (const std::exception& e) {
            LOG_ERROR("AIClient", "Failed to parse response: " + std::string(e.what()));
            response.content = "Parse Error: " + std::string(e.what());
        }

        return response;
    }

    void AIClient::ParseStreamResponse(
        const std::string& responseBody,
        StreamCallback onChunk,
        ToolCallCallback onToolCall
    ) {
        // 解析SSE格式的流式响应
        std::istringstream stream(responseBody);
        std::string line;
        std::string accumulatedContent;

        while (std::getline(stream, line)) {
            // 去除\r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) continue;

            if (line.rfind("data: ", 0) == 0) {
                std::string data = line.substr(6);
                if (data == "[DONE]") {
                    // 流结束
                    if (onChunk) {
                        onChunk("", true);
                    }
                    break;
                }

                try {
                    auto json = nlohmann::json::parse(data);
                    if (json.contains("choices") && json["choices"].is_array() && !json["choices"].empty()) {
                        auto choice = json["choices"][0];
                        auto delta = choice["delta"];

                        // 提取内容
                        if (delta.contains("content") && !delta["content"].is_null()) {
                            std::string chunk = delta["content"];
                            accumulatedContent += chunk;
                            if (onChunk) {
                                onChunk(chunk, false);
                            }
                        }

                        // 提取工具调用
                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (const auto& tc : delta["tool_calls"]) {
                                // 注意：流式工具调用可能分块发送，需要累积
                                // 简化处理：这里只处理完整的工具调用
                                if (tc.contains("function") && tc["function"].contains("name") &&
                                    tc["function"]["name"].is_string()) {
                                    ToolCall toolCall;
                                    toolCall.id = tc["id"];
                                    toolCall.type = tc["type"];
                                    toolCall.function.name = tc["function"]["name"];
                                    if (tc["function"].contains("arguments")) {
                                        toolCall.function.arguments = tc["function"]["arguments"];
                                    }
                                    if (onToolCall) {
                                        onToolCall(toolCall);
                                    }
                                }
                            }
                        }

                        // 检查finish_reason
                        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                            if (onChunk) {
                                onChunk("", true);
                            }
                        }
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("AIClient", "Failed to parse stream chunk: " + std::string(e.what()));
                    // 继续处理
                }
            }
        }
    }

    std::string AIClient::SendRequestWithRetry(
        const std::string& url,
        const nlohmann::json& body,
        int retryCount
    ) {
        int maxRetries = config_.maxRetries;
        int currentRetry = retryCount;

        while (currentRetry <= maxRetries) {
            try {
                std::string bodyStr = body.dump();

                cpr::Header headers = {
                    {"Authorization", "Bearer " + config_.apiKey},
                    {"Content-Type", "application/json"}
                };
                if (!config_.organization.empty()) {
                    headers["OpenAI-Organization"] = config_.organization;
                }

                auto response = cpr::Post(
                    cpr::Url{ url },
                    headers,
                    cpr::Body{ bodyStr },
                    cpr::Timeout{ std::chrono::seconds(config_.timeoutSeconds) },
                    cpr::Proxies{ GetProxies() }
                );

                if (response.status_code == 200) {
                    return response.text;
                }

                // 检查是否需要重试
                if (response.status_code == 429 || response.status_code >= 500) {
                    if (currentRetry < maxRetries) {
                        // 指数退避
                        int delay = static_cast<int>(std::pow(2, currentRetry)) * 1000;
                        LOG_WARN("AIClient", "Request failed with status " + std::to_string(response.status_code) +
                            ", retrying in " + std::to_string(delay) + "ms...");
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        currentRetry++;
                        continue;
                    }
                }

                LOG_ERROR("AIClient", "Request failed with status " + std::to_string(response.status_code) +
                    ": " + response.text);
                return "";
            }
            catch (const std::exception& e) {
                LOG_ERROR("AIClient", "Request exception: " + std::string(e.what()));
                if (currentRetry < maxRetries) {
                    int delay = static_cast<int>(std::pow(2, currentRetry)) * 1000;
                    LOG_WARN("AIClient", "Retrying in " + std::to_string(delay) + "ms...");
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    currentRetry++;
                    continue;
                }
                return "";
            }
        }

        LOG_ERROR("AIClient", "All retries exhausted.");
        return "";
    }

    void AIClient::SendStreamRequest(
        const std::string& url,
        const nlohmann::json& body,
        StreamCallback onChunk,
        ToolCallCallback onToolCall
    ) {
        try {
            std::string bodyStr = body.dump();

            cpr::Header headers = {
                {"Authorization", "Bearer " + config_.apiKey},
                {"Content-Type", "application/json"},
                {"Accept", "text/event-stream"}
            };
            if (!config_.organization.empty()) {
                headers["OpenAI-Organization"] = config_.organization;
            }

            // 使用cpr的流式处理
            // 由于cpr不直接支持流式响应，我们使用自定义处理
            // 这里使用libcurl的流式处理方式
            CURL* curl = curl_easy_init();
            if (!curl) {
                LOG_ERROR("AIClient", "Failed to initialize CURL.");
                if (onChunk) {
                    onChunk("Error: Failed to initialize CURL.", true);
                }
                return;
            }

            // 设置URL和Header
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            struct curl_slist* slist = nullptr;
            slist = curl_slist_append(slist, ("Authorization: Bearer " + config_.apiKey).c_str());
            slist = curl_slist_append(slist, "Content-Type: application/json");
            slist = curl_slist_append(slist, "Accept: text/event-stream");
            if (!config_.organization.empty()) {
                slist = curl_slist_append(slist, ("OpenAI-Organization: " + config_.organization).c_str());
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

            // 设置POST数据
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bodyStr.size());

            // 设置超时
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeoutSeconds);

            // 设置代理
            if (config_.proxyType != "none" && !config_.proxyHost.empty()) {
                std::string proxyUrl = config_.proxyType + "://" + config_.proxyHost;
                if (config_.proxyPort > 0) {
                    proxyUrl += ":" + std::to_string(config_.proxyPort);
                }
                curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());
            }

            // 设置流式回调
            std::string accumulatedData;
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                size_t totalSize = size * nmemb;
                std::string* data = static_cast<std::string*>(userdata);
                data->append(ptr, totalSize);
                return totalSize;
                });
            std::string responseData;
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);

            // 执行请求
            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

            curl_easy_cleanup(curl);
            curl_slist_free_all(slist);

            if (res != CURLE_OK) {
                LOG_ERROR("AIClient", "CURL error: " + std::string(curl_easy_strerror(res)));
                if (onChunk) {
                    onChunk("Error: " + std::string(curl_easy_strerror(res)), true);
                }
                return;
            }

            if (httpCode != 200) {
                LOG_ERROR("AIClient", "Stream request failed with status " + std::to_string(httpCode) +
                    ": " + responseData);
                if (onChunk) {
                    onChunk("Error: HTTP " + std::to_string(httpCode), true);
                }
                return;
            }

            // 解析流式响应
            ParseStreamResponse(responseData, onChunk, onToolCall);

        }
        catch (const std::exception& e) {
            LOG_ERROR("AIClient", "Stream request exception: " + std::string(e.what()));
            if (onChunk) {
                onChunk("Error: " + std::string(e.what()), true);
            }
        }
    }

    nlohmann::json AIClient::MessageToOpenAI(const Message& msg) {
        nlohmann::json jsonMsg;
        jsonMsg["role"] = [&]() -> std::string {
            switch (msg.role) {
            case MessageRole::System: return "system";
            case MessageRole::User: return "user";
            case MessageRole::Assistant: return "assistant";
            case MessageRole::Tool: return "tool";
            default: return "user";
            }
            }();

        if (!msg.content.empty()) {
            jsonMsg["content"] = msg.content;
        }

        if (msg.role == MessageRole::Tool) {
            jsonMsg["tool_call_id"] = msg.toolCallId;
            jsonMsg["name"] = msg.name;
        }

        // 处理附件（多模态）
        if (!msg.attachments.empty()) {
            // 如果是用户消息且包含图片，转换为多模态格式
            if (msg.role == MessageRole::User) {
                nlohmann::json contentArray = nlohmann::json::array();

                // 添加文本内容
                if (!msg.content.empty()) {
                    contentArray.push_back({
                        {"type", "text"},
                        {"text", msg.content}
                        });
                }

                // 添加图片内容
                for (const auto& attachment : msg.attachments) {
                    if (attachment.contains("type") && attachment["type"] == "image") {
                        nlohmann::json imageContent;
                        imageContent["type"] = "image_url";

                        std::string imageUrl = attachment["url"];
                        if (imageUrl.empty()) {
                            // 如果是Base64数据
                            std::string base64Data = attachment["base64"];
                            if (!base64Data.empty()) {
                                imageUrl = "data:" + attachment["mime_type"].get<std::string>() + ";base64," + base64Data;
                            }
                        }
                        imageContent["image_url"]["url"] = imageUrl;
                        contentArray.push_back(imageContent);
                    }
                }

                jsonMsg["content"] = contentArray;
            }
        }

        return jsonMsg;
    }

    nlohmann::json AIClient::ToolToOpenAI(const ToolDefinition& tool) {
        nlohmann::json jsonTool;
        jsonTool["type"] = tool.type;
        jsonTool["function"]["name"] = tool.function.name;
        jsonTool["function"]["description"] = tool.function.description;
        jsonTool["function"]["parameters"] = tool.function.parameters;
        return jsonTool;
    }

    cpr::Proxies AIClient::GetProxies() const {
        cpr::Proxies proxies;
        if (config_.proxyType != "none" && !config_.proxyHost.empty()) {
            std::string proxyUrl = config_.proxyType + "://" + config_.proxyHost;
            if (config_.proxyPort > 0) {
                proxyUrl += ":" + std::to_string(config_.proxyPort);
            }
            proxies = { {"http", proxyUrl}, {"https", proxyUrl} };
        }
        return proxies;
    }

} // namespace UnnamedBot