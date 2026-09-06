#include "../include/MessageHandler.h"
#include "../include/Database.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <regex>
#include <chrono>

namespace UnnamedBot {

    MessageHandler& MessageHandler::Instance() {
        static MessageHandler instance;
        return instance;
    }

    void MessageHandler::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("MessageHandler", "Already initialized.");
            return;
        }

        // 加载配置
        cooldownSeconds_ = ConfigManager::Instance().GetInt(
            ConfigManager::KEY_ACTIVE_COOLDOWN_SECONDS,
            480
        );

        // 加载所有会话
        auto result = Database::Instance().Query(
            "SELECT id, name, created_at, updated_at, is_active, model_config, system_prompt, pre_prompt, post_prompt "
            "FROM sessions ORDER BY updated_at DESC;"
        );

        for (const auto& row : result) {
            auto session = std::make_shared<Session>();
            session->id = std::stoll(row.at("id"));
            session->name = row.at("name");
            session->createdAt = std::stoll(row.at("created_at"));
            session->updatedAt = std::stoll(row.at("updated_at"));
            session->isActive = std::stoi(row.at("is_active")) == 1;

            if (row.find("model_config") != row.end() && !row.at("model_config").empty()) {
                try {
                    session->modelConfig = nlohmann::json::parse(row.at("model_config"));
                }
                catch (...) {
                    session->modelConfig = nlohmann::json::object();
                }
            }

            session->systemPrompt = row.at("system_prompt");
            session->prePrompt = row.at("pre_prompt");
            session->postPrompt = row.at("post_prompt");

            sessions_[session->id] = session;

            // 如果是激活会话，设为当前会话
            if (session->isActive) {
                currentSessionId_ = session->id;
                currentSession_ = session;
            }
        }

        // 如果没有会话，创建默认会话
        if (sessions_.empty()) {
            int64_t id = CreateSession("Default Session");
            if (id > 0) {
                LOG_INFO("MessageHandler", "Created default session.");
            }
        }

        // 加载当前会话消息
        if (currentSession_) {
            LoadSessionMessages(currentSessionId_);
        }

        initialized_ = true;
        LOG_INFO("MessageHandler", "Initialized with " + std::to_string(sessions_.size()) + " sessions.");
    }

    void MessageHandler::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);

        // 保存当前会话状态
        if (currentSession_) {
            SaveMessageToDB(currentSession_->messages.back());
        }

        sessions_.clear();
        currentSession_ = nullptr;
        currentSessionId_ = -1;
        initialized_ = false;

        LOG_INFO("MessageHandler", "Shut down.");
    }

    // ============ 会话管理 ============

    int64_t MessageHandler::CreateSession(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);

        int64_t now = GetCurrentTimestamp();

        // 获取默认系统提示
        std::string systemPrompt = "You are a cute desktop pet. You are friendly, helpful, and slightly playful. "
            "You can see what applications the user is running, check the weather, "
            "and capture screenshots. Respond in a warm, conversational tone.";

        int64_t id = Database::Instance().GetLastInsertRowId();

        Database::Instance().Execute(
            "INSERT INTO sessions (name, created_at, updated_at, is_active, system_prompt) "
            "VALUES (?, ?, ?, 0, ?);",
            { name, std::to_string(now), std::to_string(now), systemPrompt }
        );

        id = Database::Instance().GetLastInsertRowId();
        if (id <= 0) {
            LOG_ERROR("MessageHandler", "Failed to create session.");
            return -1;
        }

        auto session = std::make_shared<Session>();
        session->id = id;
        session->name = name;
        session->createdAt = now;
        session->updatedAt = now;
        session->isActive = false;
        session->systemPrompt = systemPrompt;
        session->modelConfig = nlohmann::json::object();

        sessions_[id] = session;

        // 切换到新会话
        SwitchSession(id);

        LOG_INFO("MessageHandler", "Created session: " + name + " (ID: " + std::to_string(id) + ")");
        return id;
    }

    bool MessageHandler::SwitchSession(int64_t sessionId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return false;
        }

        // 保存当前会话消息
        if (currentSession_ && !currentSession_->messages.empty()) {
            SaveMessageToDB(currentSession_->messages.back());
        }

        // 取消激活当前会话
        if (currentSession_) {
            Database::Instance().Execute(
                "UPDATE sessions SET is_active = 0 WHERE id = ?;",
                { std::to_string(currentSessionId_) }
            );
            currentSession_->isActive = false;
        }

        // 切换到新会话
        currentSession_ = it->second;
        currentSessionId_ = sessionId;
        currentSession_->isActive = true;
        currentSession_->messages.clear();

        // 激活新会话
        Database::Instance().Execute(
            "UPDATE sessions SET is_active = 1, updated_at = ? WHERE id = ?;",
            { std::to_string(GetCurrentTimestamp()), std::to_string(sessionId) }
        );

        // 加载消息
        LoadSessionMessages(sessionId);

        LOG_INFO("MessageHandler", "Switched to session: " + currentSession_->name + " (ID: " + std::to_string(sessionId) + ")");
        return true;
    }

    std::shared_ptr<Session> MessageHandler::GetCurrentSession() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentSession_;
    }

    std::vector<Session> MessageHandler::GetAllSessions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Session> result;
        for (const auto& pair : sessions_) {
            result.push_back(*pair.second);
        }
        return result;
    }

    bool MessageHandler::DeleteSession(int64_t sessionId) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (sessionId == currentSessionId_) {
            LOG_ERROR("MessageHandler", "Cannot delete current session.");
            return false;
        }

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return false;
        }

        // 删除数据库中的会话和关联消息
        Database::Instance().Execute(
            "DELETE FROM sessions WHERE id = ?;",
            { std::to_string(sessionId) }
        );

        sessions_.erase(it);
        LOG_INFO("MessageHandler", "Deleted session: " + std::to_string(sessionId));
        return true;
    }

    bool MessageHandler::RenameSession(int64_t sessionId, const std::string& newName) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return false;
        }

        it->second->name = newName;
        Database::Instance().Execute(
            "UPDATE sessions SET name = ? WHERE id = ?;",
            { newName, std::to_string(sessionId) }
        );

        LOG_DEBUG("MessageHandler", "Renamed session to: " + newName);
        return true;
    }

    bool MessageHandler::ClearSessionMessages(int64_t sessionId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return false;
        }

        // 删除消息
        Database::Instance().Execute(
            "DELETE FROM messages WHERE session_id = ?;",
            { std::to_string(sessionId) }
        );

        // 如果是当前会话，清空内存
        if (sessionId == currentSessionId_) {
            it->second->messages.clear();
        }

        LOG_INFO("MessageHandler", "Cleared messages for session: " + std::to_string(sessionId));
        return true;
    }

    // ============ 消息管理 ============

    int64_t MessageHandler::AddMessage(
        MessageRole role,
        const std::string& content,
        const nlohmann::json& extraData,
        const std::string& triggerSource,
        bool isActiveTrigger
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!currentSession_) {
            LOG_ERROR("MessageHandler", "No active session.");
            return -1;
        }

        ConversationMessage msg;
        msg.role = role;
        msg.content = content;
        msg.timestamp = GetCurrentTimestamp();
        msg.extraData = extraData;
        msg.triggerSource = triggerSource;
        msg.isActiveTrigger = isActiveTrigger;
        msg.sessionId = currentSessionId_;
        msg.tokenCount = EstimateTokens(content);

        // 保存到数据库
        if (!SaveMessageToDB(msg)) {
            LOG_ERROR("MessageHandler", "Failed to save message to database.");
            return -1;
        }

        // 添加到内存
        currentSession_->messages.push_back(msg);
        UpdateSessionTimestamp(currentSessionId_);

        // 触发回调
        if (newMessageCallback_) {
            newMessageCallback_(msg);
        }

        LOG_DEBUG("MessageHandler", "Added message: " + content.substr(0, 50) + "...");
        return msg.id;
    }

    int64_t MessageHandler::AddAIResponse(
        const std::string& content,
        int promptTokens,
        int completionTokens,
        const nlohmann::json& extraData
    ) {
        nlohmann::json data = extraData;
        data["prompt_tokens"] = promptTokens;
        data["completion_tokens"] = completionTokens;
        data["total_tokens"] = promptTokens + completionTokens;

        return AddMessage(MessageRole::Assistant, content, data);
    }

    std::vector<ConversationMessage> MessageHandler::GetSessionMessages(
        int64_t sessionId,
        int limit,
        int offset
    ) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return {};
        }

        if (sessionId == currentSessionId_) {
            // 从内存获取
            auto& messages = it->second->messages;
            if (limit <= 0) {
                return messages;
            }
            int start = std::max(0, static_cast<int>(messages.size()) - limit - offset);
            int end = std::min(static_cast<int>(messages.size()), start + limit);
            return std::vector<ConversationMessage>(messages.begin() + start, messages.begin() + end);
        }

        // 从数据库获取
        std::vector<ConversationMessage> result;
        std::string sql = "SELECT id, role, content, timestamp, token_count, extra_data FROM messages "
            "WHERE session_id = ? ORDER BY timestamp DESC";
        if (limit > 0) {
            sql += " LIMIT " + std::to_string(limit);
        }
        if (offset > 0) {
            sql += " OFFSET " + std::to_string(offset);
        }

        auto rows = Database::Instance().Query(sql, { std::to_string(sessionId) });
        for (const auto& row : rows) {
            ConversationMessage msg;
            msg.id = std::stoll(row.at("id"));
            msg.sessionId = sessionId;
            msg.role = static_cast<MessageRole>(std::stoi(row.at("role")));
            msg.content = row.at("content");
            msg.timestamp = std::stoll(row.at("timestamp"));
            msg.tokenCount = std::stoi(row.at("token_count"));
            if (row.find("extra_data") != row.end() && !row.at("extra_data").empty()) {
                try {
                    msg.extraData = nlohmann::json::parse(row.at("extra_data"));
                }
                catch (...) {}
            }
            result.push_back(msg);
        }

        return result;
    }

    std::vector<ConversationMessage> MessageHandler::GetCurrentMessages(int limit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentSession_) {
            return {};
        }
        return GetSessionMessages(currentSessionId_, limit);
    }

    bool MessageHandler::DeleteMessage(int64_t messageId) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 从数据库删除
        Database::Instance().Execute(
            "DELETE FROM messages WHERE id = ?;",
            { std::to_string(messageId) }
        );

        // 从内存删除
        if (currentSession_) {
            auto it = std::find_if(
                currentSession_->messages.begin(),
                currentSession_->messages.end(),
                [messageId](const ConversationMessage& msg) {
                    return msg.id == messageId;
                }
            );
            if (it != currentSession_->messages.end()) {
                currentSession_->messages.erase(it);
            }
        }

        LOG_DEBUG("MessageHandler", "Deleted message: " + std::to_string(messageId));
        return true;
    }

    std::vector<ConversationMessage> MessageHandler::GetHistoryForAI(int64_t sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto messages = GetSessionMessages(sessionId, 0);

        // 过滤掉主动触发消息（它们只是事件记录，不包含在对话历史中）
        std::vector<ConversationMessage> history;
        for (const auto& msg : messages) {
            if (!msg.isActiveTrigger || msg.role == MessageRole::User) {
                history.push_back(msg);
            }
        }

        // 应用上下文裁剪
        int maxTokens = ConfigManager::Instance().GetInt(ConfigManager::KEY_CONTEXT_MAX_TOKENS, 4096);
        int maxRounds = ConfigManager::Instance().GetInt(ConfigManager::KEY_CONTEXT_MAX_ROUNDS, 30);

        return TrimHistory(history, maxTokens, maxRounds);
    }

    // ============ 上下文管理 ============

    std::vector<ConversationMessage> MessageHandler::TrimHistory(
        const std::vector<ConversationMessage>& messages,
        int maxTokens,
        int maxRounds
    ) const {
        if (messages.empty()) {
            return {};
        }

        std::vector<ConversationMessage> result;

        // 保留系统消息（如果有）
        int startIndex = 0;
        int totalTokens = 0;
        int totalRounds = 0;

        // 从最新消息开始向前遍历
        for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
            const auto& msg = messages[i];

            // 计算Token
            int tokens = msg.tokenCount > 0 ? msg.tokenCount : EstimateTokens(msg.content);

            // 检查是否超出限制
            if (totalTokens + tokens > maxTokens || totalRounds >= maxRounds) {
                // 如果是系统消息，无论如何保留
                if (msg.role == MessageRole::System) {
                    continue;
                }
                break;
            }

            totalTokens += tokens;
            if (msg.role == MessageRole::User || msg.role == MessageRole::Assistant) {
                totalRounds++;
            }

            result.push_back(msg);
        }

        // 反转（恢复时间顺序）
        std::reverse(result.begin(), result.end());

        // 确保系统消息在最前面
        std::vector<ConversationMessage> finalResult;
        for (const auto& msg : messages) {
            if (msg.role == MessageRole::System) {
                // 检查是否已经在结果中
                auto it = std::find_if(result.begin(), result.end(),
                    [&msg](const ConversationMessage& m) { return m.id == msg.id; });
                if (it == result.end()) {
                    finalResult.push_back(msg);
                }
            }
        }

        // 添加其他消息
        for (const auto& msg : result) {
            if (msg.role != MessageRole::System) {
                finalResult.push_back(msg);
            }
        }

        LOG_DEBUG("MessageHandler", "Trimmed history from " + std::to_string(messages.size()) +
            " to " + std::to_string(finalResult.size()) + " messages.");
        return finalResult;
    }

    std::vector<Message> MessageHandler::BuildAIMessages(
        const std::vector<ConversationMessage>& messages,
        const std::string& systemPrompt,
        const std::string& prePrompt,
        const std::string& postPrompt
    ) const {
        std::vector<Message> aiMessages;

        // 添加系统提示
        if (!systemPrompt.empty() || !currentSession_->systemPrompt.empty()) {
            Message systemMsg;
            systemMsg.role = MessageRole::System;
            systemMsg.content = systemPrompt.empty() ? currentSession_->systemPrompt : systemPrompt;
            aiMessages.push_back(systemMsg);
        }

        // 添加前置提示
        if (!prePrompt.empty()) {
            Message preMsg;
            preMsg.role = MessageRole::User;
            preMsg.content = prePrompt;
            aiMessages.push_back(preMsg);
        }

        // 转换对话消息
        for (const auto& msg : messages) {
            if (msg.role == MessageRole::System) {
                continue; // 系统消息已经单独处理
            }

            Message aiMsg;
            aiMsg.role = msg.role;
            aiMsg.content = msg.content;
            aiMsg.extraData = msg.extraData;

            // 处理工具调用结果
            if (msg.role == MessageRole::Tool) {
                if (msg.extraData.contains("tool_call_id")) {
                    aiMsg.toolCallId = msg.extraData["tool_call_id"];
                }
                if (msg.extraData.contains("name")) {
                    aiMsg.name = msg.extraData["name"];
                }
            }

            aiMessages.push_back(aiMsg);
        }

        // 添加后置提示
        if (!postPrompt.empty()) {
            Message postMsg;
            postMsg.role = MessageRole::User;
            postMsg.content = postPrompt;
            aiMessages.push_back(postMsg);
        }

        return aiMessages;
    }

    // ============ 主动触发管理 ============

    bool MessageHandler::HandleTriggerEvent(const TriggerEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查冷却
        if (IsInCooldown()) {
            LOG_DEBUG("MessageHandler", "Trigger event ignored due to cooldown: " + event.description);
            return false;
        }

        // 更新冷却时间
        lastTriggerTime_ = GetCurrentTimestamp();

        // 记录触发事件
        LOG_INFO("MessageHandler", "Trigger event: " + event.description);

        // 触发回调
        if (triggerCallback_) {
            triggerCallback_(event);
        }

        // 生成AI回复（如果设置了回调）
        if (aiRequestCallback_) {
            ProcessTriggerWithAI(event);
        }

        return true;
    }

    void MessageHandler::SetTriggerCallback(TriggerCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        triggerCallback_ = callback;
    }

    void MessageHandler::SetAIRequestCallback(AIRequestCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        aiRequestCallback_ = callback;
    }

    bool MessageHandler::IsInCooldown() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (lastTriggerTime_ == 0) {
            return false;
        }

        int64_t now = GetCurrentTimestamp();
        int64_t elapsed = now - lastTriggerTime_;

        return elapsed < cooldownSeconds_;
    }

    int64_t MessageHandler::GetLastTriggerTime() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastTriggerTime_;
    }

    void MessageHandler::ResetCooldown() {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTriggerTime_ = 0;
        LOG_DEBUG("MessageHandler", "Cooldown reset.");
    }

    // ============ 事件监听 ============

    void MessageHandler::OnNewMessage(NewMessageCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        newMessageCallback_ = callback;
    }

    // ============ 搜索和导出 ============

    std::vector<ConversationMessage> MessageHandler::SearchMessages(
        int64_t sessionId,
        const std::string& keyword,
        int limit
    ) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<ConversationMessage> result;

        // 使用SQLite FTS5（如果需要更高效的搜索，可以创建FTS表）
        std::string sql = "SELECT id, role, content, timestamp, token_count, extra_data FROM messages "
            "WHERE session_id = ? AND content LIKE ? ORDER BY timestamp DESC LIMIT ?;";

        auto rows = Database::Instance().Query(sql, {
            std::to_string(sessionId),
            "%" + keyword + "%",
            std::to_string(limit)
            });

        for (const auto& row : rows) {
            ConversationMessage msg;
            msg.id = std::stoll(row.at("id"));
            msg.sessionId = sessionId;
            msg.role = static_cast<MessageRole>(std::stoi(row.at("role")));
            msg.content = row.at("content");
            msg.timestamp = std::stoll(row.at("timestamp"));
            msg.tokenCount = std::stoi(row.at("token_count"));
            if (row.find("extra_data") != row.end() && !row.at("extra_data").empty()) {
                try {
                    msg.extraData = nlohmann::json::parse(row.at("extra_data"));
                }
                catch (...) {}
            }
            result.push_back(msg);
        }

        return result;
    }

    bool MessageHandler::ExportSession(int64_t sessionId, const std::wstring& filePath) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto messages = GetSessionMessages(sessionId, 0);
        if (messages.empty()) {
            LOG_ERROR("MessageHandler", "No messages to export for session: " + std::to_string(sessionId));
            return false;
        }

        nlohmann::json exportData;
        exportData["session_id"] = sessionId;
        exportData["session_name"] = sessions_.at(sessionId)->name;
        exportData["export_time"] = GetCurrentTimestamp();

        nlohmann::json messagesArray = nlohmann::json::array();
        for (const auto& msg : messages) {
            nlohmann::json msgJson;
            msgJson["role"] = static_cast<int>(msg.role);
            msgJson["content"] = msg.content;
            msgJson["timestamp"] = msg.timestamp;
            msgJson["token_count"] = msg.tokenCount;
            if (!msg.extraData.empty()) {
                msgJson["extra_data"] = msg.extraData;
            }
            if (!msg.triggerSource.empty()) {
                msgJson["trigger_source"] = msg.triggerSource;
            }
            msgJson["is_active_trigger"] = msg.isActiveTrigger;
            messagesArray.push_back(msgJson);
        }
        exportData["messages"] = messagesArray;

        try {
            std::ofstream file(filePath);
            if (!file.is_open()) {
                LOG_ERROR("MessageHandler", "Failed to open file for export: " + std::filesystem::path(filePath).u8string());
                return false;
            }
            file << std::setw(2) << exportData << std::endl;
            LOG_INFO("MessageHandler", "Exported session to: " + std::filesystem::path(filePath).u8string());
            return true;
        }
        catch (const std::exception& e) {
            LOG_ERROR("MessageHandler", "Export failed: " + std::string(e.what()));
            return false;
        }
    }

    bool MessageHandler::ImportSession(const std::wstring& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            std::ifstream file(filePath);
            if (!file.is_open()) {
                LOG_ERROR("MessageHandler", "Failed to open file for import: " + std::filesystem::path(filePath).u8string());
                return false;
            }

            nlohmann::json importData;
            file >> importData;

            if (!importData.contains("messages") || !importData["messages"].is_array()) {
                LOG_ERROR("MessageHandler", "Invalid import file format.");
                return false;
            }

            // 创建新会话
            std::string sessionName = "Imported_" + std::to_string(GetCurrentTimestamp());
            if (importData.contains("session_name")) {
                sessionName = importData["session_name"] + "_imported";
            }

            int64_t sessionId = CreateSession(sessionName);
            if (sessionId < 0) {
                LOG_ERROR("MessageHandler", "Failed to create session for import.");
                return false;
            }

            // 导入消息
            for (const auto& msgJson : importData["messages"]) {
                MessageRole role = static_cast<MessageRole>(msgJson["role"].get<int>());
                std::string content = msgJson["content"];
                nlohmann::json extraData = msgJson.contains("extra_data") ? msgJson["extra_data"] : nlohmann::json::object();
                std::string triggerSource = msgJson.contains("trigger_source") ? msgJson["trigger_source"] : "";
                bool isActiveTrigger = msgJson.contains("is_active_trigger") ? msgJson["is_active_trigger"].get<bool>() : false;

                // 切换会话以导入
                SwitchSession(sessionId);
                AddMessage(role, content, extraData, triggerSource, isActiveTrigger);
            }

            LOG_INFO("MessageHandler", "Imported session from: " + std::filesystem::path(filePath).u8string());
            return true;
        }
        catch (const std::exception& e) {
            LOG_ERROR("MessageHandler", "Import failed: " + std::string(e.what()));
            return false;
        }
    }

    // ============ 配置 ============

    void MessageHandler::UpdateSessionConfig(int64_t sessionId, const nlohmann::json& config) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return;
        }

        it->second->modelConfig = config;

        Database::Instance().Execute(
            "UPDATE sessions SET model_config = ?, updated_at = ? WHERE id = ?;",
            { config.dump(), std::to_string(GetCurrentTimestamp()), std::to_string(sessionId) }
        );
    }

    nlohmann::json MessageHandler::GetSessionConfig(int64_t sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            LOG_ERROR("MessageHandler", "Session not found: " + std::to_string(sessionId));
            return {};
        }

        return it->second->modelConfig;
    }

    // ============ 私有方法 ============

    bool MessageHandler::SaveMessageToDB(const ConversationMessage& message) {
        std::string roleStr = std::to_string(static_cast<int>(message.role));
        std::string extraDataStr = message.extraData.empty() ? "{}" : message.extraData.dump();

        // 生成新ID
        Database::Instance().Execute(
            "INSERT INTO messages (session_id, role, content, timestamp, token_count, extra_data) "
            "VALUES (?, ?, ?, ?, ?, ?);",
            {
                std::to_string(message.sessionId),
                roleStr,
                message.content,
                std::to_string(message.timestamp),
                std::to_string(message.tokenCount),
                extraDataStr
            }
        );

        int64_t id = Database::Instance().GetLastInsertRowId();
        if (id > 0) {
            const_cast<ConversationMessage&>(message).id = id;
            return true;
        }

        return false;
    }

    void MessageHandler::LoadSessionMessages(int64_t sessionId) {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            return;
        }

        it->second->messages.clear();

        auto rows = Database::Instance().Query(
            "SELECT id, role, content, timestamp, token_count, extra_data FROM messages "
            "WHERE session_id = ? ORDER BY timestamp ASC;",
            { std::to_string(sessionId) }
        );

        for (const auto& row : rows) {
            ConversationMessage msg;
            msg.id = std::stoll(row.at("id"));
            msg.sessionId = sessionId;
            msg.role = static_cast<MessageRole>(std::stoi(row.at("role")));
            msg.content = row.at("content");
            msg.timestamp = std::stoll(row.at("timestamp"));
            msg.tokenCount = std::stoi(row.at("token_count"));
            if (row.find("extra_data") != row.end() && !row.at("extra_data").empty()) {
                try {
                    msg.extraData = nlohmann::json::parse(row.at("extra_data"));
                }
                catch (...) {}
            }
            it->second->messages.push_back(msg);
        }

        LOG_DEBUG("MessageHandler", "Loaded " + std::to_string(it->second->messages.size()) +
            " messages for session: " + std::to_string(sessionId));
    }

    int64_t MessageHandler::GetCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }

    int MessageHandler::EstimateTokens(const std::string& text) const {
        // 粗略估算：每4个字符约等于1个token
        // 实际应使用tiktoken等更精确的库
        return static_cast<int>(text.length() / 4.0) + 1;
    }

    void MessageHandler::UpdateSessionTimestamp(int64_t sessionId) {
        Database::Instance().Execute(
            "UPDATE sessions SET updated_at = ? WHERE id = ?;",
            { std::to_string(GetCurrentTimestamp()), std::to_string(sessionId) }
        );
    }

    void MessageHandler::ProcessTriggerWithAI(const TriggerEvent& event) {
        if (!aiRequestCallback_) {
            LOG_WARN("MessageHandler", "AI request callback not set.");
            return;
        }

        // 构建触发消息
        std::string triggerMessage = "Trigger event: " + event.description;
        if (!event.data.empty()) {
            triggerMessage += "\nData: " + event.data.dump();
        }

        // 添加用户消息（作为主动触发的上下文）
        AddMessage(MessageRole::User, triggerMessage, event.data, "trigger_" + std::to_string(static_cast<int>(event.type)), true);

        // 获取历史消息
        auto history = GetHistoryForAI(currentSessionId_);

        // 调用AI生成回复
        std::vector<ConversationMessage> aiMessages;
        std::string aiResponse = aiRequestCallback_(history, aiMessages);

        // 添加AI回复
        if (!aiResponse.empty()) {
            AddAIResponse(aiResponse);
        }

        // 保存AI中间消息（如果有）
        for (const auto& msg : aiMessages) {
            if (msg.role == MessageRole::Assistant || msg.role == MessageRole::Tool) {
                AddMessage(msg.role, msg.content, msg.extraData);
            }
        }
    }

} // namespace UnnamedBot