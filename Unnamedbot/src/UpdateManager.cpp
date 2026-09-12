#include "../include/UpdateManager.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <cpr/cpr.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <shellapi.h>

namespace UnnamedBot {

    UpdateManager& UpdateManager::Instance() {
        static UpdateManager instance;
        return instance;
    }

    void UpdateManager::Initialize() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // 获取当前版本
        currentVersion_ = GetVersionFromResource();

        // 从配置加载设置
        autoCheckEnabled_ = ConfigManager::Instance().GetBool(ConfigManager::KEY_AUTO_UPDATE_CHECK, true);
        checkIntervalHours_ = ConfigManager::Instance().GetInt(ConfigManager::KEY_UPDATE_CHECK_INTERVAL_HOURS, 24);

        LOG_INFO("UpdateManager", "Initialized. Current version: " + currentVersion_);
    }

    void UpdateManager::Shutdown() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        latestUpdate_.reset();
        status_ = UpdateStatus::Idle;
        LOG_INFO("UpdateManager", "Shut down.");
    }

    bool UpdateManager::CheckUpdate() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!autoCheckEnabled_) {
            LOG_DEBUG("UpdateManager", "Auto check disabled.");
            return false;
        }

        // 检查频率限制
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        int64_t intervalSeconds = checkIntervalHours_ * 3600;
        if (now - lastCheckTime_ < intervalSeconds && lastCheckTime_ > 0) {
            LOG_DEBUG("UpdateManager", "Check interval not reached.");
            return false;
        }

        lastCheckTime_ = now;
        status_ = UpdateStatus::Checking;
        LOG_INFO("UpdateManager", "Checking for updates...");

        try {
            nlohmann::json updateData = FetchUpdateInfo();
            if (updateData.empty()) {
                status_ = UpdateStatus::Idle;
                LOG_DEBUG("UpdateManager", "No update info found.");
                return false;
            }

            // 解析更新信息
            auto updateInfo = std::make_shared<UpdateInfo>();
            updateInfo->version = updateData["tag_name"];
            updateInfo->releaseDate = updateData["published_at"];
            updateInfo->changelog = updateData["body"];

            // 获取下载URL
            if (updateData.contains("assets") && updateData["assets"].is_array()) {
                for (const auto& asset : updateData["assets"]) {
                    std::string name = asset["name"];
                    if (name.find(".exe") != std::string::npos || name.find(".msi") != std::string::npos) {
                        updateInfo->downloadUrl = asset["browser_download_url"];
                        updateInfo->size = asset["size"];
                        break;
                    }
                }
            }

            // 检查是否已跳过该版本
            if (IsVersionSkipped(updateInfo->version)) {
                LOG_DEBUG("UpdateManager", "Version " + updateInfo->version + " is skipped.");
                status_ = UpdateStatus::Idle;
                return false;
            }

            // 比较版本
            if (CompareVersion(updateInfo->version, currentVersion_)) {
                // 有新版本
                latestUpdate_ = updateInfo;
                status_ = UpdateStatus::UpdateAvailable;
                LOG_INFO("UpdateManager", "New version available: " + updateInfo->version);

                if (onUpdateAvailable_) {
                    onUpdateAvailable_(*updateInfo);
                }
                return true;
            }
            else {
                LOG_DEBUG("UpdateManager", "No new version available.");
                status_ = UpdateStatus::Idle;
                return false;
            }

        }
        catch (const std::exception& e) {
            status_ = UpdateStatus::Error;
            lastError_ = "Check update failed: " + std::string(e.what());
            LOG_ERROR("UpdateManager", lastError_);
            return false;
        }
    }

    std::shared_ptr<UpdateInfo> UpdateManager::GetLatestUpdate() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return latestUpdate_;
    }

    bool UpdateManager::DownloadUpdate(const std::string& url, const std::wstring& savePath) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (url.empty() || savePath.empty()) {
            lastError_ = "Invalid download URL or save path.";
            LOG_ERROR("UpdateManager", lastError_);
            return false;
        }

        status_ = UpdateStatus::Downloading;
        LOG_INFO("UpdateManager", "Downloading update from: " + url);

        try {
            // 创建下载目录
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(savePath).parent_path(), ec);

            // 使用cpr下载
            std::ofstream file(savePath, std::ios::binary);
            if (!file.is_open()) {
                lastError_ = "Failed to open file for writing.";
                LOG_ERROR("UpdateManager", lastError_);
                return false;
            }

            auto response = cpr::Download(
                file,
                cpr::Url{ url },
                cpr::ProgressCallback([this](cpr::cpr_pf_arg_t downloadTotal, cpr::cpr_pf_arg_t downloadNow,
                                             cpr::cpr_pf_arg_t /*uploadTotal*/, cpr::cpr_pf_arg_t /*uploadNow*/,
                                             intptr_t /*userdata*/) {
                    if (onDownloadProgress_) {
                        onDownloadProgress_(static_cast<int64_t>(downloadNow), static_cast<int64_t>(downloadTotal));
                    }
                    return true;
                    }),
                cpr::Timeout{ std::chrono::seconds(300) } // 5分钟超时
            );

            file.close();

            if (response.status_code == 200) {
                status_ = UpdateStatus::Downloaded;
                LOG_INFO("UpdateManager", "Update downloaded: " + Utils::PathToUtf8(savePath));
                return true;
            }
            else {
                lastError_ = "Download failed. HTTP " + std::to_string(response.status_code);
                LOG_ERROR("UpdateManager", lastError_);
                status_ = UpdateStatus::Error;
                return false;
            }

        }
        catch (const std::exception& e) {
            lastError_ = "Download exception: " + std::string(e.what());
            LOG_ERROR("UpdateManager", lastError_);
            status_ = UpdateStatus::Error;
            return false;
        }
    }

    bool UpdateManager::InstallUpdate(const std::wstring& updatePath) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!std::filesystem::exists(updatePath)) {
            lastError_ = "Update file not found: " + Utils::PathToUtf8(updatePath);
            LOG_ERROR("UpdateManager", lastError_);
            return false;
        }

        status_ = UpdateStatus::Installing;
        LOG_INFO("UpdateManager", "Installing update: " + Utils::PathToUtf8(updatePath));

        if (!ExecuteInstaller(updatePath)) {
            status_ = UpdateStatus::Error;
            LOG_ERROR("UpdateManager", "Installation failed: " + lastError_);
            return false;
        }

        status_ = UpdateStatus::Installed;
        LOG_INFO("UpdateManager", "Update installed successfully.");

        if (onUpdateInstalled_) {
            onUpdateInstalled_();
        }

        return true;
    }

    void UpdateManager::OnUpdateAvailable(std::function<void(const UpdateInfo&)> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onUpdateAvailable_ = callback;
    }

    void UpdateManager::OnDownloadProgress(std::function<void(int64_t, int64_t)> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onDownloadProgress_ = callback;
    }

    void UpdateManager::OnUpdateInstalled(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onUpdateInstalled_ = callback;
    }

    std::string UpdateManager::GetCurrentVersion() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return currentVersion_;
    }

    void UpdateManager::SetUpdateServerUrl(const std::string& url) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        updateServerUrl_ = url;
        LOG_DEBUG("UpdateManager", "Update server URL: " + url);
    }

    void UpdateManager::SetAutoCheck(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        autoCheckEnabled_ = enabled;
        ConfigManager::Instance().SetBool(ConfigManager::KEY_AUTO_UPDATE_CHECK, enabled);
    }

    bool UpdateManager::IsAutoCheckEnabled() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return autoCheckEnabled_;
    }

    void UpdateManager::SkipVersion(const std::string& version) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!IsVersionSkipped(version)) {
            skippedVersions_.push_back(version);
            LOG_DEBUG("UpdateManager", "Skipped version: " + version);
        }
    }

    bool UpdateManager::IsVersionSkipped(const std::string& version) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& v : skippedVersions_) {
            if (v == version) {
                return true;
            }
        }
        return false;
    }

    // ============ 私有方法 ============

    nlohmann::json UpdateManager::FetchUpdateInfo() {
        try {
            auto response = cpr::Get(
                cpr::Url{ updateServerUrl_ },
                cpr::Timeout{ std::chrono::seconds(10) },
                cpr::Header{ {"Accept", "application/json"} }
            );

            if (response.status_code == 200) {
                return nlohmann::json::parse(response.text);
            }
            else {
                LOG_WARN("UpdateManager", "Failed to fetch update info. HTTP: " + std::to_string(response.status_code));
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("UpdateManager", "Fetch update info exception: " + std::string(e.what()));
        }

        return {};
    }

    bool UpdateManager::CompareVersion(const std::string& v1, const std::string& v2) const {
        // 简单版本比较：按.分割，逐段比较
        auto parseVersion = [](const std::string& v) {
            std::vector<int> parts;
            std::string str = v;
            // 移除前缀 'v'
            if (!str.empty() && str[0] == 'v') {
                str = str.substr(1);
            }
            size_t pos = 0;
            while ((pos = str.find('.')) != std::string::npos) {
                try {
                    parts.push_back(std::stoi(str.substr(0, pos)));
                }
                catch (...) {
                    parts.push_back(0);
                }
                str = str.substr(pos + 1);
            }
            if (!str.empty()) {
                try {
                    parts.push_back(std::stoi(str));
                }
                catch (...) {
                    parts.push_back(0);
                }
            }
            return parts;
            };

        auto parts1 = parseVersion(v1);
        auto parts2 = parseVersion(v2);

        size_t maxSize = (std::max)(parts1.size(), parts2.size());
        for (size_t i = 0; i < maxSize; ++i) {
            int p1 = i < parts1.size() ? parts1[i] : 0;
            int p2 = i < parts2.size() ? parts2[i] : 0;
            if (p1 > p2) return true;
            if (p1 < p2) return false;
        }

        return false; // 相等
    }

    std::string UpdateManager::GetVersionFromResource() const {
        // 从资源文件获取版本号（简化：使用硬编码）
        // 实际应从VERSIONINFO资源读取
        return "0.1.0";
    }

    bool UpdateManager::ExecuteInstaller(const std::wstring& installerPath) {
        // 使用ShellExecute执行安装程序
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(SHELLEXECUTEINFOW);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpFile = installerPath.c_str();
        sei.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&sei)) {
            // 注意：本类有同名成员 GetLastError()（返回 lastError_），必须用 :: 限定调用 Win32 API
            lastError_ = "Failed to execute installer. Error: " + std::to_string(::GetLastError());
            LOG_ERROR("UpdateManager", lastError_);
            return false;
        }

        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }

        return true;
    }

} // namespace UnnamedBot