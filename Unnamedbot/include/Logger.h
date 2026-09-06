#pragma once
 
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <memory>

namespace UnnamedBot {

    // 日志级别
    enum class LogLevel : uint8_t {
        Debug = 0,
        Info,
        Warn,
        Error
    };

    // 日志系统（单例）
    class Logger {
    public:
        static Logger& Instance();

        // 初始化日志系统，指定日志目录和保留天数（默认30天）
        void Initialize(const std::wstring& logDir = L"./logs", int retainDays = 30);

        // 设置最低输出级别（低于该级别不输出），默认Info
        void SetMinLevel(LogLevel level);

        // 设置是否同时输出到控制台（Debug模式），默认true
        void SetConsoleOutput(bool enable);

        // 记录日志
        void Log(LogLevel level, const std::string& module, const std::string& message);

        // 便捷宏定义（推荐使用）
#define LOG_DEBUG(mod, msg) UnnamedBot::Logger::Instance().Log(UnnamedBot::LogLevel::Debug, mod, msg)
#define LOG_INFO(mod, msg)  UnnamedBot::Logger::Instance().Log(UnnamedBot::LogLevel::Info,  mod, msg)
#define LOG_WARN(mod, msg)  UnnamedBot::Logger::Instance().Log(UnnamedBot::LogLevel::Warn,  mod, msg)
#define LOG_ERROR(mod, msg) UnnamedBot::Logger::Instance().Log(UnnamedBot::LogLevel::Error, mod, msg)

    private:
        Logger() = default;
        ~Logger();

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // 获取当前日志文件路径（按天）
        std::filesystem::path GetLogFilePath(const std::chrono::system_clock::time_point& tp);

        // 清理过期日志
        void CleanOldLogs();

        // 写入文件
        void WriteToFile(const std::string& formatted);

        std::wstring logDir_;
        int retainDays_ = 30;
        LogLevel minLevel_ = LogLevel::Info;
        bool consoleOutput_ = true;
        std::mutex mutex_;

        // 当前日志文件流
        std::unique_ptr<std::ofstream> fileStream_;
        std::chrono::system_clock::time_point currentFileDate_; // 用于判断日期是否改变
    };

} // namespace UnnamedBot