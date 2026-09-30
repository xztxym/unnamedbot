#include "../include/ConfigManager.h"
#include "../include/Database.h"
#include "../include/Logger.h"
#include "../include/Utils.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>

namespace UnnamedBot {

    // 定义静态常量
    const std::string ConfigManager::KEY_API_BASE_URL = "api.base_url";
    const std::string ConfigManager::KEY_API_KEY = "api.key";
    const std::string ConfigManager::KEY_API_ORG_ID = "api.org_id";
    const std::string ConfigManager::KEY_API_MODEL_DEFAULT = "api.model_default";

    const std::string ConfigManager::KEY_SAMPLING_TEMPERATURE = "sampling.temperature";
    const std::string ConfigManager::KEY_SAMPLING_TOP_P = "sampling.top_p";
    const std::string ConfigManager::KEY_SAMPLING_FREQUENCY_PENALTY = "sampling.frequency_penalty";
    const std::string ConfigManager::KEY_SAMPLING_PRESENCE_PENALTY = "sampling.presence_penalty";

    const std::string ConfigManager::KEY_STREAM_ENABLED = "stream.enabled";
    const std::string ConfigManager::KEY_STREAM_TIMEOUT_SECONDS = "stream.timeout_seconds";

    const std::string ConfigManager::KEY_ACTIVE_INTERVAL_PROCESS = "active.interval_process";
    const std::string ConfigManager::KEY_ACTIVE_INTERVAL_WEATHER = "active.interval_weather";
    const std::string ConfigManager::KEY_ACTIVE_INTERVAL_SCREENSHOT = "active.interval_screenshot";
    const std::string ConfigManager::KEY_ACTIVE_IP_CHECK_ON_START = "active.ip_check_on_start";
    const std::string ConfigManager::KEY_ACTIVE_COOLDOWN_SECONDS = "active.cooldown_seconds";

    const std::string ConfigManager::KEY_AMAP_API_KEY = "amap.api_key";

    const std::string ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP = "feature.always_on_top";
    const std::string ConfigManager::KEY_FEATURE_AUTO_HIDE_FULLSCREEN = "feature.auto_hide_fullscreen";
    const std::string ConfigManager::KEY_FEATURE_DRAG_ENABLED = "feature.drag_enabled";
    const std::string ConfigManager::KEY_FEATURE_CLICK_THROUGH = "feature.click_through";

    const std::string ConfigManager::KEY_QUICK_ENABLED = "quick.enabled";
    const std::string ConfigManager::KEY_QUICK_MODE = "quick.mode";

    const std::string ConfigManager::KEY_PROMPT_ENABLED = "prompt.enabled";

    const std::string ConfigManager::KEY_MODEL_DEFAULT_ID = "model.default_id";
    const std::string ConfigManager::KEY_MODEL_DEFAULT_SCALE = "model.default_scale";
    const std::string ConfigManager::KEY_MODEL_DEFAULT_OPACITY = "model.default_opacity";

    const std::string ConfigManager::KEY_CONTEXT_MAX_TOKENS = "context.max_tokens";
    const std::string ConfigManager::KEY_CONTEXT_MAX_ROUNDS = "context.max_rounds";

    const std::string ConfigManager::KEY_ATTACHMENT_MAX_SIZE_MB = "attachment.max_size_mb";
    const std::string ConfigManager::KEY_ATTACHMENT_TOTAL_QUOTA_MB = "attachment.total_quota_mb";
    const std::string ConfigManager::KEY_ATTACHMENT_RETENTION_DAYS = "attachment.retention_days";

    const std::string ConfigManager::KEY_LOGS_RETENTION_DAYS = "logs.retention_days";

    const std::string ConfigManager::KEY_UI_THEME = "ui.theme";
    const std::string ConfigManager::KEY_HOTKEY_TOGGLE_MESSAGEBOX = "hotkey.toggle_messagebox";
    const std::string ConfigManager::KEY_HOTKEY_TOGGLE_TOPMOST = "hotkey.toggle_topmost";

    const std::string ConfigManager::KEY_PROXY_TYPE = "proxy.type";
    const std::string ConfigManager::KEY_PROXY_HOST = "proxy.host";
    const std::string ConfigManager::KEY_PROXY_PORT = "proxy.port";

    const std::string ConfigManager::KEY_AUTO_UPDATE_CHECK = "update.auto_check";
    const std::string ConfigManager::KEY_UPDATE_CHECK_INTERVAL_HOURS = "update.check_interval_hours";

    // 实现

    ConfigManager& ConfigManager::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        static ConfigManager* instance = new ConfigManager();
        return *instance;
    }

    void ConfigManager::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_) return;

        // 确保数据库已初始化
        if (!Database::Instance().Initialize()) {
            LOG_ERROR("ConfigManager", "Database not initialized, cannot load config.");
            return;
        }

        // 检查数据库中是否有任何配置项
        auto result = Database::Instance().Query("SELECT COUNT(*) as cnt FROM config;");
        if (!result.empty() && std::stoi(result[0]["cnt"]) == 0) {
            // 无配置，写入默认值
            WriteDefaults();
            LOG_INFO("ConfigManager", "Default configuration written.");
        }

        LoadAll();
        loaded_ = true;
        LOG_INFO("ConfigManager", "Configuration loaded successfully.");
    }

    std::string ConfigManager::GetString(const std::string& key, const std::string& defaultValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            // 尝试加载
            // 但为防止递归，调用Initialize会加锁，此处简单处理：直接查询数据库
            // 但我们有缓存，先尝试从缓存获取，若没有则直接查库
            auto it = cache_.find(key);
            if (it != cache_.end()) return it->second;
            // 否则查数据库
            auto result = Database::Instance().Query("SELECT value FROM config WHERE key = ?;", { key });
            if (!result.empty()) {
                std::string val = result[0]["value"];
                cache_[key] = val;
                return val;
            }
            return defaultValue;
        }
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        return defaultValue;
    }

    int ConfigManager::GetInt(const std::string& key, int defaultValue) {
        std::string val = GetString(key, "");
        if (val.empty()) return defaultValue;
        try { return std::stoi(val); }
        catch (...) { return defaultValue; }
    }

    double ConfigManager::GetDouble(const std::string& key, double defaultValue) {
        std::string val = GetString(key, "");
        if (val.empty()) return defaultValue;
        try { return std::stod(val); }
        catch (...) { return defaultValue; }
    }

    bool ConfigManager::GetBool(const std::string& key, bool defaultValue) {
        std::string val = GetString(key, "");
        if (val.empty()) return defaultValue;
        return (val == "true" || val == "1" || val == "yes");
    }

    void ConfigManager::SetString(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 更新数据库
        int64_t now = GetCurrentTimestamp();
        Database::Instance().Execute(
            "INSERT OR REPLACE INTO config (key, value, updated_at) VALUES (?, ?, ?);",
            { key, value, std::to_string(now) }
        );
        // 更新缓存
        cache_[key] = value;
        LOG_DEBUG("ConfigManager", "Set config " + key + " = " + value);
    }

    void ConfigManager::SetInt(const std::string& key, int value) {
        SetString(key, std::to_string(value));
    }

    void ConfigManager::SetDouble(const std::string& key, double value) {
        SetString(key, std::to_string(value));
    }

    void ConfigManager::SetBool(const std::string& key, bool value) {
        SetString(key, value ? "true" : "false");
    }

    std::map<std::string, std::string> ConfigManager::GetAllConfigs() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            // 尝试加载
            LoadAll();
            loaded_ = true;
        }
        return cache_;
    }

    bool ConfigManager::ExportToJson(const std::wstring& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadAll();
            loaded_ = true;
        }
        nlohmann::json jsonObj;
        for (const auto& pair : cache_) {
            jsonObj[pair.first] = pair.second;
        }
        try {
            std::ofstream file(filePath);
            if (!file.is_open()) {
                LOG_ERROR("ConfigManager", "Failed to open file for export: " + Utils::PathToUtf8(filePath));
                return false;
            }
            file << std::setw(4) << jsonObj << std::endl;
            LOG_INFO("ConfigManager", "Config exported to " + Utils::PathToUtf8(filePath));
            return true;
        }
        catch (const std::exception& e) {
            LOG_ERROR("ConfigManager", "Export failed: " + std::string(e.what()));
            return false;
        }
    }

    bool ConfigManager::ImportFromJson(const std::wstring& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            std::ifstream file(filePath);
            if (!file.is_open()) {
                LOG_ERROR("ConfigManager", "Failed to open file for import: " + Utils::PathToUtf8(filePath));
                return false;
            }
            nlohmann::json jsonObj;
            file >> jsonObj;
            if (!jsonObj.is_object()) {
                LOG_ERROR("ConfigManager", "Invalid JSON format.");
                return false;
            }
            Database::Instance().BeginTransaction();
            int64_t now = GetCurrentTimestamp();
            for (auto& [key, value] : jsonObj.items()) {
                if (value.is_string()) {
                    Database::Instance().Execute(
                        "INSERT OR REPLACE INTO config (key, value, updated_at) VALUES (?, ?, ?);",
                        { key, value.get<std::string>(), std::to_string(now) }
                    );
                    cache_[key] = value.get<std::string>();
                }
                else {
                    // 其他类型转字符串
                    std::string strVal = value.dump();
                    Database::Instance().Execute(
                        "INSERT OR REPLACE INTO config (key, value, updated_at) VALUES (?, ?, ?);",
                        { key, strVal, std::to_string(now) }
                    );
                    cache_[key] = strVal;
                }
            }
            Database::Instance().CommitTransaction();
            LOG_INFO("ConfigManager", "Config imported from " + Utils::PathToUtf8(filePath));
            return true;
        }
        catch (const std::exception& e) {
            LOG_ERROR("ConfigManager", "Import failed: " + std::string(e.what()));
            Database::Instance().RollbackTransaction();
            return false;
        }
    }

    void ConfigManager::RestoreDefaults() {
        std::lock_guard<std::mutex> lock(mutex_);
        // 清空配置表
        Database::Instance().Execute("DELETE FROM config;");
        cache_.clear();
        // 写入默认
        WriteDefaults();
        LoadAll();
        LOG_INFO("ConfigManager", "Configuration restored to defaults.");
    }

    void ConfigManager::LoadAll() {
        auto result = Database::Instance().Query("SELECT key, value FROM config;");
        cache_.clear();
        for (const auto& row : result) {
            auto it = row.find("key");
            auto itVal = row.find("value");
            if (it != row.end() && itVal != row.end()) {
                cache_[it->second] = itVal->second;
            }
        }
    }

    int64_t ConfigManager::GetCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }

    void ConfigManager::WriteDefaults() {
        // 使用SetString会触发事务，逐个写入
        // 但为防止日志过多，直接执行SQL批量插入
        int64_t now = GetCurrentTimestamp();
        std::vector<std::pair<std::string, std::string>> defaults = {
            {KEY_API_BASE_URL, "https://api.openai.com/v1"},
            {KEY_API_KEY, ""},
            {KEY_API_ORG_ID, ""},
            {KEY_API_MODEL_DEFAULT, "gpt-4o"},
            {KEY_SAMPLING_TEMPERATURE, "1.0"},
            {KEY_SAMPLING_TOP_P, "1.0"},
            {KEY_SAMPLING_FREQUENCY_PENALTY, "0.0"},
            {KEY_SAMPLING_PRESENCE_PENALTY, "0.0"},
            {KEY_STREAM_ENABLED, "true"},
            {KEY_STREAM_TIMEOUT_SECONDS, "30"},
            {KEY_ACTIVE_INTERVAL_PROCESS, "60"},        // 60秒
            {KEY_ACTIVE_INTERVAL_WEATHER, "300"},       // 5分钟
            {KEY_ACTIVE_INTERVAL_SCREENSHOT, "1800"},   // 30分钟
            {KEY_ACTIVE_IP_CHECK_ON_START, "true"},
            {KEY_ACTIVE_COOLDOWN_SECONDS, "480"},       // 8分钟
            {KEY_AMAP_API_KEY, ""},
            {KEY_FEATURE_ALWAYS_ON_TOP, "true"},
            {KEY_FEATURE_AUTO_HIDE_FULLSCREEN, "true"},
            {KEY_FEATURE_DRAG_ENABLED, "true"},
            {KEY_FEATURE_CLICK_THROUGH, "true"},
            {KEY_QUICK_ENABLED, "false"},
            {KEY_QUICK_MODE, "关闭"},
            {KEY_PROMPT_ENABLED, ""},
            {KEY_MODEL_DEFAULT_ID, "Haru"},
            {KEY_MODEL_DEFAULT_SCALE, "1.0"},
            {KEY_MODEL_DEFAULT_OPACITY, "1.0"},
            {KEY_CONTEXT_MAX_TOKENS, "4096"},
            {KEY_CONTEXT_MAX_ROUNDS, "30"},
            {KEY_ATTACHMENT_MAX_SIZE_MB, "20"},
            {KEY_ATTACHMENT_TOTAL_QUOTA_MB, "500"},
            {KEY_ATTACHMENT_RETENTION_DAYS, "30"},
            {KEY_LOGS_RETENTION_DAYS, "30"},
            {KEY_UI_THEME, "system"},
            {KEY_HOTKEY_TOGGLE_MESSAGEBOX, "Ctrl+Alt+M"},
            {KEY_HOTKEY_TOGGLE_TOPMOST, "Ctrl+Alt+T"},
            {KEY_PROXY_TYPE, "none"},
            {KEY_PROXY_HOST, ""},
            {KEY_PROXY_PORT, "0"},
            {KEY_AUTO_UPDATE_CHECK, "true"},
            {KEY_UPDATE_CHECK_INTERVAL_HOURS, "24"}
        };

        Database::Instance().BeginTransaction();
        for (const auto& pair : defaults) {
            Database::Instance().Execute(
                "INSERT OR REPLACE INTO config (key, value, updated_at) VALUES (?, ?, ?);",
                { pair.first, pair.second, std::to_string(now) }
            );
        }
        Database::Instance().CommitTransaction();
    }

} // namespace UnnamedBot