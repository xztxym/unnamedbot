#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>

namespace UnnamedBot {

    // 附件信息
    struct Attachment {
        int64_t id = 0;
        int64_t messageId = 0;
        int64_t sessionId = 0;
        std::string filename;
        std::string filepath;
        std::string mimeType;
        int64_t size = 0;
        int64_t uploadedAt = 0;
    };

    // 附件管理器
    class AttachmentManager {
    public:
        static AttachmentManager& Instance();

        // 初始化
        void Initialize();

        // 关闭
        void Shutdown();

        // 添加附件
        bool AddAttachment(
            int64_t messageId,
            int64_t sessionId,
            const std::wstring& sourcePath,
            const std::string& mimeType = ""
        );

        // 添加附件（从内存数据）
        bool AddAttachmentFromData(
            int64_t messageId,
            int64_t sessionId,
            const std::string& filename,
            const std::vector<BYTE>& data,
            const std::string& mimeType = ""
        );

        // 获取附件
        std::shared_ptr<Attachment> GetAttachment(int64_t attachmentId) const;

        // 获取消息的所有附件
        std::vector<std::shared_ptr<Attachment>> GetAttachmentsForMessage(int64_t messageId) const;

        // 获取会话的所有附件
        std::vector<std::shared_ptr<Attachment>> GetAttachmentsForSession(int64_t sessionId) const;

        // 删除附件
        bool DeleteAttachment(int64_t attachmentId);

        // 删除消息的所有附件
        bool DeleteAttachmentsForMessage(int64_t messageId);

        // 获取附件路径
        std::wstring GetAttachmentPath(const Attachment& attachment) const;

        // 检查附件是否存在
        bool AttachmentExists(const Attachment& attachment) const;

        // 获取总附件大小
        int64_t GetTotalAttachmentSize() const;

        // 清理过期附件
        int CleanupOldAttachments(int retentionDays = 30);

        // 清理超出配额的附件
        int CleanupByQuota(int64_t maxTotalSizeMB = 500);

        // 验证附件大小
        bool ValidateSize(int64_t size) const;

        // 验证文件类型
        bool ValidateFileType(const std::string& filename) const;

        // 获取允许的文件扩展名列表
        std::vector<std::string> GetAllowedExtensions() const;

        // 设置允许的文件扩展名
        void SetAllowedExtensions(const std::vector<std::string>& extensions);

    private:
        AttachmentManager() = default;
        ~AttachmentManager() = default;
        AttachmentManager(const AttachmentManager&) = delete;
        AttachmentManager& operator=(const AttachmentManager&) = delete;

        // 保存附件到数据库
        bool SaveAttachmentToDB(Attachment& attachment);

        // 从数据库加载附件
        void LoadAttachmentsFromDB();

        // 生成存储路径
        std::wstring GenerateStoragePath(int64_t sessionId, const std::string& filename) const;

        // 检查目录配额
        bool CheckQuota(int64_t additionalSize) const;

        // 获取当前时间戳
        int64_t GetCurrentTimestamp() const;

        // 附件缓存
        std::map<int64_t, std::shared_ptr<Attachment>> attachments_;
        mutable std::mutex mutex_;

        // 配置
        std::wstring attachmentRoot_ = L"./attachments";
        int64_t maxSizeMB_ = 20;      // 单个文件最大20MB
        int64_t totalQuotaMB_ = 500;  // 总配额500MB
        std::vector<std::string> allowedExtensions_;
        bool initialized_ = false;

        // 默认允许的扩展名
        static const std::vector<std::string> DEFAULT_ALLOWED_EXTENSIONS;
    };

} // namespace UnnamedBot