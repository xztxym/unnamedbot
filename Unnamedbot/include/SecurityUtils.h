#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace UnnamedBot {

    // 安全工具类（DPAPI加密）
    class SecurityUtils {
    public:
        static SecurityUtils& Instance();

        // 初始化
        void Initialize();

        // 加密字符串（使用DPAPI）
        std::string Encrypt(const std::string& plainText);

        // 解密字符串（使用DPAPI）
        std::string Decrypt(const std::string& cipherText);

        // 加密二进制数据
        std::vector<BYTE> EncryptData(const std::vector<BYTE>& data);

        // 解密二进制数据
        std::vector<BYTE> DecryptData(const std::vector<BYTE>& data);

        // 检查是否已初始化
        bool IsInitialized() const { return initialized_; }

    private:
        SecurityUtils() = default;
        ~SecurityUtils() = default;
        SecurityUtils(const SecurityUtils&) = delete;
        SecurityUtils& operator=(const SecurityUtils&) = delete;

        // 使用DPAPI加密/解密
        std::vector<BYTE> CryptData(
            const std::vector<BYTE>& data,
            bool encrypt,
            const std::string& optionalEntropy = ""
        );

        bool initialized_ = false;
        mutable std::mutex mutex_;
    };

} // namespace UnnamedBot