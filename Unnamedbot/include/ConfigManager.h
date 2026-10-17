#pragma once
 
#include <string>
#include <map>
#include <functional>
#include <mutex>
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
        // 图片分析（主动触发里的截屏巡检）用的模型。
        // DeepSeek 的视觉能力就挂在 deepseek-flash 上（旧名 deepseek-v4-flash-vision-exp
        // 已并入），所以默认值给它；换成别的服务商时在这里改。
        static const std::string KEY_API_MODEL_VISION;

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

        // 快速模式：关闭 Live2D 渲染，只保留一个可交互的圆点
        static const std::string KEY_QUICK_ENABLED;
        static const std::string KEY_QUICK_MODE;

        // 启用的提示词文件清单（位于 ./prompts，逗号分隔的文件名）
        static const std::string KEY_PROMPT_ENABLED;

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

        // ---- 性能相关（面向较老设备）----
        // 帧间隔：默认 100ms(10FPS)；老机器 / 低配模式调大即可显著降 CPU 与 GPU 占用
        static const std::string KEY_PERF_FRAME_INTERVAL_MS;
        // 命中掩码每 N 帧读回一次：GPU→CPU 同步读回是每帧最贵的一步
        static const std::string KEY_PERF_HITMASK_EVERY;
        // 状态落库间隔：默认 30 秒（原为 5 秒，老机器磁盘写入明显）
        static const std::string KEY_PERF_SAVE_INTERVAL;
        // 低配模式：一键把上面几项调到省电档，并关闭控制台日志
        static const std::string KEY_PERF_LOW_END;
        // 日志级别（debug/info/warn/error）与控制台输出
        static const std::string KEY_LOG_LEVEL;
        static const std::string KEY_LOG_CONSOLE;

        // ---- 系统弹窗（右下角托盘气泡）----
        // 总开关；关掉后所有系统弹窗都不再出现（桌宠气泡与控制中枢不受影响）
        static const std::string KEY_NOTIFY_ENABLED;
        // 每条新消息都弹一条太吵，单独给一个开关
        static const std::string KEY_NOTIFY_NEW_MESSAGE;

        // ---- 小范围操控（鼠标 / 窗口 / 键盘）----
        // 都是「小动作」：位移被限制在设定像素内，键盘不含功能键
        static const std::string KEY_TOOL_MOUSE_ENABLED;
        static const std::string KEY_TOOL_MOUSE_MAX_PX;
        static const std::string KEY_TOOL_WINDOW_ENABLED;
        static const std::string KEY_TOOL_WINDOW_MAX_PX;
        static const std::string KEY_TOOL_KEYBOARD_ENABLED;
        // 回复气泡贴哪儿：window=吸附活动窗口（默认）, cursor=吸附鼠标
        static const std::string KEY_UI_REPLY_ANCHOR;
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