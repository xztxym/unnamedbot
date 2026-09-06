#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

    // 更新信息
    struct UpdateInfo {
        std::string version;
        std::string releaseDate;
        std::string downloadUrl;
        std::string changelog;
        int64_t size = 0;
        bool isMandatory = false;
    };

    // 更新管理器
    class UpdateManager {
    public:
        static UpdateManager& Instance();

        // 初始化
        void Initialize();

        // 关闭
        void Shutdown();

        // 检查更新
        bool CheckUpdate();

        // 获取最新更新信息
        std::shared_ptr<UpdateInfo> GetLatestUpdate() const;

        // 下载更新
        bool DownloadUpdate(const std::string& url, const std::wstring& savePath);

        // 安装更新
        bool InstallUpdate(const std::wstring& updatePath);

        // 设置更新检查回调
        void OnUpdateAvailable(std::function<void(const UpdateInfo&)> callback);
        void OnDownloadProgress(std::function<void(int64_t, int64_t)> callback);
        void OnUpdateInstalled(std::function<void()> callback);

        // 获取当前版本
        std::string GetCurrentVersion() const;

        // 设置更新服务器URL
        void SetUpdateServerUrl(const std::string& url);

        // 启用/禁用自动检查
        void SetAutoCheck(bool enabled);
        bool IsAutoCheckEnabled() const;

        // 跳过版本
        void SkipVersion(const std::string& version);
        bool IsVersionSkipped(const std::string& version) const;

        // 获取更新状态
        enum class UpdateStatus {
            Idle,
            Checking,
            UpdateAvailable,
            Downloading,
            Downloaded,
            Installing,
            Installed,
            Error
        };
        UpdateStatus GetStatus() const { return status_; }

        // 获取错误信息
        std::string GetLastError() const { return lastError_; }

    private:
        UpdateManager() = default;
        ~UpdateManager() = default;
        UpdateManager(const UpdateManager&) = delete;
        UpdateManager& operator=(const UpdateManager&) = delete;

        // 从服务器获取更新信息
        nlohmann::json FetchUpdateInfo();

        // 解析版本号
        bool CompareVersion(const std::string& v1, const std::string& v2) const;

        // 获取当前版本（从资源文件）
        std::string GetVersionFromResource() const;

        // 执行更新安装
        bool ExecuteInstaller(const std::wstring& installerPath);

        // 配置
        std::string updateServerUrl_ = "https://api.github.com/repos/username/UnnamedBot/releases/latest";
        std::string currentVersion_ = "0.1.0";
        bool autoCheckEnabled_ = true;
        int checkIntervalHours_ = 24;

        // 更新信息
        std::shared_ptr<UpdateInfo> latestUpdate_;
        std::vector<std::string> skippedVersions_;
        UpdateStatus status_ = UpdateStatus::Idle;
        std::string lastError_;

        // 回调
        std::function<void(const UpdateInfo&)> onUpdateAvailable_;
        std::function<void(int64_t, int64_t)> onDownloadProgress_;
        std::function<void()> onUpdateInstalled_;

        // 线程安全
        mutable std::mutex mutex_;

        // 上次检查时间
        int64_t lastCheckTime_ = 0;
    };

} // namespace UnnamedBot