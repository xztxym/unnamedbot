#include "../include/AttachmentManager.h"
#include "../include/Database.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <codecvt>
#include <locale>

namespace UnnamedBot {

    const std::vector<std::string> AttachmentManager::DEFAULT_ALLOWED_EXTENSIONS = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp",
        ".txt", ".pdf", ".doc", ".docx", ".xls", ".xlsx",
        ".zip", ".rar", ".7z",
        ".mp3", ".wav", ".flac",
        ".mp4", ".avi", ".mkv"
    };

    // GetMimeType 定义于文件末尾，这里前置声明以供上方使用
    std::string GetMimeType(const std::string& filename);

    AttachmentManager& AttachmentManager::Instance() {
        static AttachmentManager instance;
        return instance;
    }

    void AttachmentManager::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            return;
        }

        // 从配置加载设置
        maxSizeMB_ = ConfigManager::Instance().GetInt(ConfigManager::KEY_ATTACHMENT_MAX_SIZE_MB, 20);
        totalQuotaMB_ = ConfigManager::Instance().GetInt(ConfigManager::KEY_ATTACHMENT_TOTAL_QUOTA_MB, 500);

        // 创建附件根目录
        std::error_code ec;
        std::filesystem::create_directories(attachmentRoot_, ec);
        if (ec) {
            LOG_ERROR("AttachmentManager", "Failed to create attachment root: " + ec.message());
        }

        // 加载现有附件
        LoadAttachmentsFromDB();

        // 清理过期附件
        int retentionDays = ConfigManager::Instance().GetInt(ConfigManager::KEY_ATTACHMENT_RETENTION_DAYS, 30);
        CleanupOldAttachments(retentionDays);

        // 清理超出配额
        CleanupByQuota(totalQuotaMB_);

        initialized_ = true;
        LOG_INFO("AttachmentManager", "Initialized with " + std::to_string(attachments_.size()) + " attachments.");
    }

    void AttachmentManager::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        attachments_.clear();
        initialized_ = false;
        LOG_INFO("AttachmentManager", "Shut down.");
    }

    bool AttachmentManager::AddAttachment(
        int64_t messageId,
        int64_t sessionId,
        const std::wstring& sourcePath,
        const std::string& mimeType
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("AttachmentManager", "Not initialized.");
            return false;
        }

        // 检查文件是否存在
        if (!std::filesystem::exists(sourcePath)) {
            LOG_ERROR("AttachmentManager", "Source file not found: " + Utils::PathToUtf8(sourcePath));
            return false;
        }

        // 获取文件信息
        std::error_code ec;
        auto fileSize = std::filesystem::file_size(sourcePath, ec);
        if (ec) {
            LOG_ERROR("AttachmentManager", "Failed to get file size: " + ec.message());
            return false;
        }

        // 验证大小
        if (!ValidateSize(fileSize)) {
            LOG_ERROR("AttachmentManager", "File size exceeds limit: " + std::to_string(fileSize));
            return false;
        }

        // 验证文件类型
        std::string filename = Utils::PathToUtf8(std::filesystem::path(sourcePath).filename());
        if (!ValidateFileType(filename)) {
            LOG_WARN("AttachmentManager", "File type not allowed: " + filename);
            // 不阻止，仅警告
        }

        // 检查配额
        if (!CheckQuota(fileSize)) {
            LOG_ERROR("AttachmentManager", "Quota exceeded.");
            return false;
        }

        // 生成存储路径
        std::wstring destPath = GenerateStoragePath(sessionId, filename);
        std::wstring destDir = std::filesystem::path(destPath).parent_path();

        // 创建目录
        std::filesystem::create_directories(destDir, ec);
        if (ec) {
            LOG_ERROR("AttachmentManager", "Failed to create directory: " + ec.message());
            return false;
        }

        // 复制文件
        try {
            std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                LOG_ERROR("AttachmentManager", "Failed to copy file: " + ec.message());
                return false;
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("AttachmentManager", "Failed to copy file: " + std::string(e.what()));
            return false;
        }

        // 创建附件记录
        Attachment attachment;
        attachment.messageId = messageId;
        attachment.sessionId = sessionId;
        attachment.filename = filename;
        attachment.filepath = Utils::PathToUtf8(destPath);
        attachment.mimeType = mimeType.empty() ? GetMimeType(filename) : mimeType;
        attachment.size = fileSize;
        attachment.uploadedAt = GetCurrentTimestamp();

        if (!SaveAttachmentToDB(attachment)) {
            // 删除已复制的文件
            std::filesystem::remove(destPath, ec);
            LOG_ERROR("AttachmentManager", "Failed to save attachment to database.");
            return false;
        }

        attachments_[attachment.id] = std::make_shared<Attachment>(attachment);
        LOG_INFO("AttachmentManager", "Added attachment: " + filename + " (" + std::to_string(fileSize) + " bytes)");
        return true;
    }

    bool AttachmentManager::AddAttachmentFromData(
        int64_t messageId,
        int64_t sessionId,
        const std::string& filename,
        const std::vector<BYTE>& data,
        const std::string& mimeType
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("AttachmentManager", "Not initialized.");
            return false;
        }

        // 验证大小
        if (!ValidateSize(static_cast<int64_t>(data.size()))) {
            LOG_ERROR("AttachmentManager", "Data size exceeds limit: " + std::to_string(data.size()));
            return false;
        }

        // 验证文件类型
        if (!ValidateFileType(filename)) {
            LOG_WARN("AttachmentManager", "File type not allowed: " + filename);
        }

        // 检查配额
        if (!CheckQuota(static_cast<int64_t>(data.size()))) {
            LOG_ERROR("AttachmentManager", "Quota exceeded.");
            return false;
        }

        // 生成存储路径
        std::wstring destPath = GenerateStoragePath(sessionId, filename);
        std::wstring destDir = std::filesystem::path(destPath).parent_path();

        // 创建目录
        std::error_code ec;
        std::filesystem::create_directories(destDir, ec);
        if (ec) {
            LOG_ERROR("AttachmentManager", "Failed to create directory: " + ec.message());
            return false;
        }

        // 写入文件
        try {
            std::ofstream file(destPath, std::ios::binary);
            if (!file.is_open()) {
                LOG_ERROR("AttachmentManager", "Failed to open file for writing.");
                return false;
            }
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            file.close();
        }
        catch (const std::exception& e) {
            LOG_ERROR("AttachmentManager", "Failed to write file: " + std::string(e.what()));
            return false;
        }

        // 创建附件记录
        Attachment attachment;
        attachment.messageId = messageId;
        attachment.sessionId = sessionId;
        attachment.filename = filename;
        attachment.filepath = Utils::PathToUtf8(destPath);
        attachment.mimeType = mimeType.empty() ? GetMimeType(filename) : mimeType;
        attachment.size = static_cast<int64_t>(data.size());
        attachment.uploadedAt = GetCurrentTimestamp();

        if (!SaveAttachmentToDB(attachment)) {
            std::filesystem::remove(destPath, ec);
            LOG_ERROR("AttachmentManager", "Failed to save attachment to database.");
            return false;
        }

        attachments_[attachment.id] = std::make_shared<Attachment>(attachment);
        LOG_INFO("AttachmentManager", "Added attachment from data: " + filename);
        return true;
    }

    std::shared_ptr<Attachment> AttachmentManager::GetAttachment(int64_t attachmentId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = attachments_.find(attachmentId);
        if (it != attachments_.end()) {
            return it->second;
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<Attachment>> AttachmentManager::GetAttachmentsForMessage(int64_t messageId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::shared_ptr<Attachment>> result;
        for (const auto& pair : attachments_) {
            if (pair.second->messageId == messageId) {
                result.push_back(pair.second);
            }
        }
        return result;
    }

    std::vector<std::shared_ptr<Attachment>> AttachmentManager::GetAttachmentsForSession(int64_t sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::shared_ptr<Attachment>> result;
        for (const auto& pair : attachments_) {
            if (pair.second->sessionId == sessionId) {
                result.push_back(pair.second);
            }
        }
        return result;
    }

    bool AttachmentManager::DeleteAttachment(int64_t attachmentId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = attachments_.find(attachmentId);
        if (it == attachments_.end()) {
            LOG_WARN("AttachmentManager", "Attachment not found: " + std::to_string(attachmentId));
            return false;
        }

        // 删除文件
        std::wstring filepath = std::filesystem::path(it->second->filepath).wstring();
        std::error_code ec;
        if (std::filesystem::exists(filepath, ec)) {
            std::filesystem::remove(filepath, ec);
            if (ec) {
                LOG_WARN("AttachmentManager", "Failed to delete file: " + ec.message());
            }
        }

        // 删除数据库记录
        Database::Instance().Execute(
            "DELETE FROM attachments WHERE id = ?;",
            { std::to_string(attachmentId) }
        );

        attachments_.erase(it);
        LOG_DEBUG("AttachmentManager", "Deleted attachment: " + std::to_string(attachmentId));
        return true;
    }

    bool AttachmentManager::DeleteAttachmentsForMessage(int64_t messageId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto attachments = GetAttachmentsForMessage(messageId);
        for (const auto& att : attachments) {
            DeleteAttachment(att->id);
        }
        return true;
    }

    std::wstring AttachmentManager::GetAttachmentPath(const Attachment& attachment) const {
        return std::filesystem::path(attachment.filepath).wstring();
    }

    bool AttachmentManager::AttachmentExists(const Attachment& attachment) const {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(attachment.filepath), ec);
    }

    int64_t AttachmentManager::GetTotalAttachmentSize() const {
        std::lock_guard<std::mutex> lock(mutex_);

        int64_t totalSize = 0;
        for (const auto& pair : attachments_) {
            totalSize += pair.second->size;
        }
        return totalSize;
    }

    int AttachmentManager::CleanupOldAttachments(int retentionDays) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (retentionDays < 0) return 0;

        int64_t cutoff = GetCurrentTimestamp() - (retentionDays * 24 * 3600);
        std::vector<int64_t> toDelete;

        for (const auto& pair : attachments_) {
            if (pair.second->uploadedAt < cutoff) {
                toDelete.push_back(pair.first);
            }
        }

        for (int64_t id : toDelete) {
            DeleteAttachment(id);
        }

        if (!toDelete.empty()) {
            LOG_INFO("AttachmentManager", "Cleaned up " + std::to_string(toDelete.size()) + " old attachments.");
        }
        return static_cast<int>(toDelete.size());
    }

    int AttachmentManager::CleanupByQuota(int64_t maxTotalSizeMB) {
        std::lock_guard<std::mutex> lock(mutex_);

        int64_t maxSizeBytes = maxTotalSizeMB * 1024 * 1024;
        int64_t currentSize = GetTotalAttachmentSize();

        if (currentSize <= maxSizeBytes) {
            return 0;
        }

        // 按上传时间排序（最旧的先删除）
        std::vector<std::pair<int64_t, int64_t>> sorted;
        for (const auto& pair : attachments_) {
            sorted.push_back({ pair.second->uploadedAt, pair.first });
        }
        std::sort(sorted.begin(), sorted.end());

        int deleted = 0;
        for (const auto& pair : sorted) {
            if (currentSize <= maxSizeBytes) {
                break;
            }
            auto att = GetAttachment(pair.second);
            if (att) {
                currentSize -= att->size;
                DeleteAttachment(pair.second);
                deleted++;
            }
        }

        if (deleted > 0) {
            LOG_INFO("AttachmentManager", "Cleaned up " + std::to_string(deleted) + " attachments by quota.");
        }
        return deleted;
    }

    bool AttachmentManager::ValidateSize(int64_t size) const {
        int64_t maxSizeBytes = maxSizeMB_ * 1024 * 1024;
        return size <= maxSizeBytes;
    }

    bool AttachmentManager::ValidateFileType(const std::string& filename) const {
        if (allowedExtensions_.empty()) {
            return true; // 没有限制
        }

        const std::string ext = Utils::ToLower(Utils::PathToUtf8(std::filesystem::path(filename).extension()));

        for (const auto& allowed : allowedExtensions_) {
            std::string allowedLower = Utils::ToLower(allowed);
            if (ext == allowedLower) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> AttachmentManager::GetAllowedExtensions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allowedExtensions_;
    }

    void AttachmentManager::SetAllowedExtensions(const std::vector<std::string>& extensions) {
        std::lock_guard<std::mutex> lock(mutex_);
        allowedExtensions_ = extensions;
    }

    // ============ 私有方法 ============

    bool AttachmentManager::SaveAttachmentToDB(Attachment& attachment) {
        Database::Instance().Execute(
            "INSERT INTO attachments (message_id, session_id, filename, filepath, mime_type, size, uploaded_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);",
            {
                std::to_string(attachment.messageId),
                std::to_string(attachment.sessionId),
                attachment.filename,
                attachment.filepath,
                attachment.mimeType,
                std::to_string(attachment.size),
                std::to_string(attachment.uploadedAt)
            }
        );

        attachment.id = Database::Instance().GetLastInsertRowId();
        return attachment.id > 0;
    }

    void AttachmentManager::LoadAttachmentsFromDB() {
        auto rows = Database::Instance().Query(
            "SELECT id, message_id, session_id, filename, filepath, mime_type, size, uploaded_at FROM attachments;"
        );

        attachments_.clear();
        for (const auto& row : rows) {
            auto attachment = std::make_shared<Attachment>();
            attachment->id = std::stoll(row.at("id"));
            attachment->messageId = std::stoll(row.at("message_id"));
            attachment->sessionId = std::stoll(row.at("session_id"));
            attachment->filename = row.at("filename");
            attachment->filepath = row.at("filepath");
            attachment->mimeType = row.at("mime_type");
            attachment->size = std::stoll(row.at("size"));
            attachment->uploadedAt = std::stoll(row.at("uploaded_at"));

            attachments_[attachment->id] = attachment;
        }
    }

    std::wstring AttachmentManager::GenerateStoragePath(int64_t sessionId, const std::string& filename) const {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_s(&tm_now, &time_t_now);

        wchar_t dateStr[32];
        wcsftime(dateStr, 32, L"%Y-%m-%d", &tm_now);

        std::wstring destPath = attachmentRoot_ + L"/" +
            std::to_wstring(sessionId) + L"/" +
            dateStr + L"/" +
            std::wstring(filename.begin(), filename.end());

        // 如果文件已存在，添加数字后缀
        std::wstring originalPath = destPath;
        int counter = 1;
        std::error_code ec;
        while (std::filesystem::exists(destPath, ec)) {
            std::wstring ext = std::filesystem::path(originalPath).extension();
            std::wstring stem = std::filesystem::path(originalPath).stem();
            destPath = std::filesystem::path(originalPath).parent_path().wstring() + L"/" +
                stem + L"_" + std::to_wstring(counter++) + ext;
        }

        return destPath;
    }

    bool AttachmentManager::CheckQuota(int64_t additionalSize) const {
        int64_t currentSize = GetTotalAttachmentSize();
        int64_t maxSizeBytes = totalQuotaMB_ * 1024 * 1024;
        return (currentSize + additionalSize) <= maxSizeBytes;
    }

    int64_t AttachmentManager::GetCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }

    std::string GetMimeType(const std::string& filename) {
        const std::string ext = Utils::ToLower(Utils::PathToUtf8(std::filesystem::path(filename).extension()));

        static std::map<std::string, std::string> mimeTypes = {
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".png", "image/png"},
            {".gif", "image/gif"},
            {".bmp", "image/bmp"},
            {".webp", "image/webp"},
            {".txt", "text/plain"},
            {".pdf", "application/pdf"},
            {".doc", "application/msword"},
            {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {".xls", "application/vnd.ms-excel"},
            {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {".zip", "application/zip"},
            {".rar", "application/x-rar-compressed"},
            {".7z", "application/x-7z-compressed"},
            {".mp3", "audio/mpeg"},
            {".wav", "audio/wav"},
            {".flac", "audio/flac"},
            {".mp4", "video/mp4"},
            {".avi", "video/x-msvideo"},
            {".mkv", "video/x-matroska"}
        };

        auto it = mimeTypes.find(ext);
        return it != mimeTypes.end() ? it->second : "application/octet-stream";
    }

} // namespace UnnamedBot