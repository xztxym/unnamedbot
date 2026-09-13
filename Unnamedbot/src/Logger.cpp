#include "../include/Logger.h"
#include "../include/Utils.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <Windows.h>
#include <vector>

namespace UnnamedBot {

    Logger& Logger::Instance() {
        // 故意不随静态存储期销毁：日志在静态析构阶段仍会被调用
        // （例如 AppCore 析构触发 Renderer::Shutdown 时写日志），
        // 若此时 Logger 已销毁，就是向已释放的流写入而崩溃。
        static Logger* instance = new Logger();
        return *instance;
    }

    Logger::~Logger() {
        if (fileStream_ && fileStream_->is_open()) {
            fileStream_->close();
        }
    }

    void Logger::Initialize(const std::wstring& logDir, int retainDays) {
        std::lock_guard<std::mutex> lock(mutex_);
        logDir_ = logDir;
        retainDays_ = retainDays;
        // 创建目录
        std::error_code ec;
        std::filesystem::create_directories(logDir_, ec);
        // 清理旧日志（异步？先同步做）
        CleanOldLogs();
        // 打开当天的日志文件
        auto now = std::chrono::system_clock::now();
        currentFileDate_ = now;
        auto path = GetLogFilePath(now);
        fileStream_ = std::make_unique<std::ofstream>(path, std::ios::app);
        if (!fileStream_->is_open()) {
            // 如果文件无法打开，至少控制台输出
            std::cerr << "[Logger] Failed to open log file: " << Utils::PathToUtf8(path) << std::endl;
        }
    }

    void Logger::SetMinLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        minLevel_ = level;
    }

    void Logger::SetConsoleOutput(bool enable) {
        std::lock_guard<std::mutex> lock(mutex_);
        consoleOutput_ = enable;
    }

    void Logger::Log(LogLevel level, const std::string& module, const std::string& message) {
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_s(&tm_now, &time_t_now);

        // 格式化时间
        std::ostringstream time_oss;
        time_oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
        std::string timeStr = time_oss.str();

        // 级别字符串
        const char* levelStr = nullptr;
        switch (level) {
        case LogLevel::Debug: levelStr = "DEBUG"; break;
        case LogLevel::Info:  levelStr = "INFO";  break;
        case LogLevel::Warn:  levelStr = "WARN";  break;
        case LogLevel::Error: levelStr = "ERROR"; break;
        }

        // 组装消息: [时间] [级别] [模块] 消息
        std::ostringstream formatted;
        formatted << "[" << timeStr << "] [" << levelStr << "] [" << module << "] " << message;

        std::string output = formatted.str();

        std::lock_guard<std::mutex> lock(mutex_);

        // 最低级别过滤（minLevel_ 由 SetMinLevel 在锁内写入，这里必须在锁内读取）
        if (level < minLevel_) return;

        // 输出到控制台
        if (consoleOutput_) {
            HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hConsole != INVALID_HANDLE_VALUE && hConsole != nullptr) {
                // 简单颜色（可选）
                WORD color = 7; // 默认白
                if (level == LogLevel::Error) color = 4;  // 红
                else if (level == LogLevel::Warn) color = 6; // 黄
                else if (level == LogLevel::Debug) color = 8; // 灰
                SetConsoleTextAttribute(hConsole, color);
                std::cout << output << std::endl;
                SetConsoleTextAttribute(hConsole, 7); // 恢复
            }
            else {
                std::cout << output << std::endl;
            }
        }

        // 写入文件（确保文件流有效）
        if (!fileStream_ || !fileStream_->is_open()) {
            // 尝试重新打开当天文件
            auto now2 = std::chrono::system_clock::now();
            auto path = GetLogFilePath(now2);
            fileStream_ = std::make_unique<std::ofstream>(path, std::ios::app);
            if (!fileStream_->is_open()) return;
        }

        // 检查日期是否变更，若变更则关闭旧文件并打开新文件
        auto nowDate = std::chrono::system_clock::now();
        auto oldDate = currentFileDate_;
        auto oldDay = std::chrono::floor<std::chrono::days>(oldDate);
        auto newDay = std::chrono::floor<std::chrono::days>(nowDate);
        if (newDay != oldDay) {
            // 日期变了，关闭旧文件，打开新文件
            if (fileStream_->is_open()) fileStream_->close();
            currentFileDate_ = nowDate;
            auto newPath = GetLogFilePath(nowDate);
            fileStream_ = std::make_unique<std::ofstream>(newPath, std::ios::app);
            if (!fileStream_->is_open()) {
                // 无法打开，记错
                std::cerr << "[Logger] Failed to open new log file: " << Utils::PathToUtf8(newPath) << std::endl;
                return;
            }
            // 清理过期日志（每天切换时清理一次）
            CleanOldLogs();
        }

        *fileStream_ << output << std::endl;
        if (fileStream_->fail()) {
            // 写入失败，忽略
        }
    }

    std::filesystem::path Logger::GetLogFilePath(const std::chrono::system_clock::time_point& tp) {
        auto time_t_tp = std::chrono::system_clock::to_time_t(tp);
        struct tm tm_tp;
        localtime_s(&tm_tp, &time_t_tp);
        std::ostringstream oss;
        oss << "app_" << std::put_time(&tm_tp, "%Y-%m-%d") << ".log";
        std::string filename = oss.str();
        return std::filesystem::path(logDir_) / filename;
    }

    void Logger::CleanOldLogs() {
        // 遍历log目录，删除修改时间早于保留天数的日志文件
        if (retainDays_ < 0) return; // 负值表示不清理

        auto now = std::chrono::system_clock::now();
        auto cutoff = now - std::chrono::hours(24 * retainDays_);

        std::error_code ec;
        if (!std::filesystem::exists(logDir_, ec)) return;

        for (auto& entry : std::filesystem::directory_iterator(logDir_, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            auto filename = Utils::PathToUtf8(entry.path().filename());
            // 只删除 app_YYYY-MM-DD.log 格式文件
            if (filename.rfind("app_", 0) == 0 && filename.length() >= 15 && filename.substr(filename.length() - 4) == ".log") {
                auto lastWrite = entry.last_write_time(ec);
                // std::filesystem 使用 file_clock，与 system_clock 无保证的可移植换算，
                // 这里统一使用 Win32 API 获取文件修改时间。
                HANDLE hFile = CreateFileW(entry.path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE) continue;
                FILETIME ftWrite;
                if (GetFileTime(hFile, nullptr, nullptr, &ftWrite)) {
                    // 转换FILETIME到SYSTEMTIME再到time_t
                    SYSTEMTIME st;
                    FileTimeToSystemTime(&ftWrite, &st);
                    // 转换SYSTEMTIME到time_t
                    struct tm tm = { 0 };
                    tm.tm_year = st.wYear - 1900;
                    tm.tm_mon = st.wMonth - 1;
                    tm.tm_mday = st.wDay;
                    tm.tm_hour = st.wHour;
                    tm.tm_min = st.wMinute;
                    tm.tm_sec = st.wSecond;
                    time_t fileTime = mktime(&tm);
                    if (fileTime != -1) {
                        if (fileTime < std::chrono::system_clock::to_time_t(cutoff)) {
                            // 删除文件
                            std::filesystem::remove(entry.path(), ec);
                        }
                    }
                }
                CloseHandle(hFile);
            }
        }
    }

} // namespace UnnamedBot