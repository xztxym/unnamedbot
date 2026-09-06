#pragma once
 
#include <string>
#include <map>
#include <functional>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

    // 配置管理器（单例）
    class ConfigManager {
    public:
        static ConfigManager& Instance();

        // 初始化：从数据库加载配置，若数据库为空则写入默认配置
        void Initialize();

        // 读取配置值（字符串）
        std::string GetString(const std::string& key, const std::string& defaultValue = "");

        // 读取配置值（整数）
        int GetInt(const std::string& key, int defaultValue = 0);

        // 读取配置值（浮点数）
        double GetDouble(const std::string& key, double defaultValue = 0.0);

        // 读取配置值（布尔）
        bool GetBool(const std::string& key, bool defaultValue = false);

        // 设置配置值（字符串），自动更新数据库
        void SetString(const std::string& key, const std::string& value);

        // 设置配置值（整数）
        void SetInt(const std::string& key, int value);

        // 设置配置值（浮点数）
        void SetDouble(const std::string& key, double value);

        // 设置配置值（布尔）
        void SetBool(const std::string& key, bool value);

        // 获取所有配置（以键值对形式）
        std::map<std::string, std::string> GetAllConfigs();

        // 导出配置到JSON文件
        bool ExportToJson(const std::wstring& filePath);

        // 从JSON文件导入配置（合并，覆盖现有键）
        bool ImportFromJson(const std::wstring& filePath);

        // 恢复默认配置（清空并重新写入）
        void RestoreDefaults();

    private:
        ConfigManager() = default;
        ~ConfigManager() = default;
        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;

        // 加载所有配置到缓存
        void LoadAll();

        // 获取当前时间戳（秒）
        int64_t GetCurrentTimestamp();

        // 写入默认配置（若数据库中无配置）
        void WriteDefaults();

        // 缓存
        std::map<std::string, std::string> cache_;
        bool loaded_ = false;
        std::mutex mutex_;

        // 默认配置键定义（公共静态常量，便于外部使用）
    public:
        // API配置
        static const std::string KEY_API_BASE_URL;
        static const std::string KEY_API_KEY;
        static const std::string KEY_API_ORG_ID;
        static const std::string KEY_API_MODEL_DEFAULT;

        // 采样参数
        static const std::string KEY_SAMPLING_TEMPERATURE;
        static const std::string KEY_SAMPLING_TOP_P;
        static const std::string KEY_SAMPLING_FREQUENCY_PENALTY;
        static const std::string KEY_SAMPLING_PRESENCE_PENALTY;

        // 流式
        static const std::string KEY_STREAM_ENABLED;
        static const std::string KEY_STREAM_TIMEOUT_SECONDS;

        // 主动触发
        static const std::string KEY_ACTIVE_INTERVAL_PROCESS;  // 秒，-1禁用
        static const std::string KEY_ACTIVE_INTERVAL_WEATHER;
        static const std::string KEY_ACTIVE_INTERVAL_SCREENSHOT;
        static const std::string KEY_ACTIVE_IP_CHECK_ON_START;
        static const std::string KEY_ACTIVE_COOLDOWN_SECONDS;

        // 高德天气API Key
        static const std::string KEY_AMAP_API_KEY;

        // 隐私与功能开关
        static const std::string KEY_FEATURE_ALWAYS_ON_TOP;
        static const std::string KEY_FEATURE_AUTO_HIDE_FULLSCREEN;
        static const std::string KEY_FEATURE_DRAG_ENABLED;
        static const std::string KEY_FEATURE_CLICK_THROUGH;

        // 模型状态默认值
        static const std::string KEY_MODEL_DEFAULT_ID;
        static const std::string KEY_MODEL_DEFAULT_SCALE;
        static const std::string KEY_MODEL_DEFAULT_OPACITY;

        // 上下文长度
        static const std::string KEY_CONTEXT_MAX_TOKENS;
        static const std::string KEY_CONTEXT_MAX_ROUNDS;

        // 其他
        static const std::string KEY_ATTACHMENT_MAX_SIZE_MB;
        static const std::string KEY_ATTACHMENT_TOTAL_QUOTA_MB;
        static const std::string KEY_ATTACHMENT_RETENTION_DAYS;
        static const std::string KEY_LOGS_RETENTION_DAYS;
        static const std::string KEY_UI_THEME;  // "light", "dark", "system"
        static const std::string KEY_HOTKEY_TOGGLE_MESSAGEBOX;
        static const std::string KEY_HOTKEY_TOGGLE_TOPMOST;
        static const std::string KEY_PROXY_TYPE; // "none", "http", "socks5"
        static const std::string KEY_PROXY_HOST;
        static const std::string KEY_PROXY_PORT;
        static const std::string KEY_AUTO_UPDATE_CHECK;
        static const std::string KEY_UPDATE_CHECK_INTERVAL_HOURS;
    };

} // namespace UnnamedBot