#include "../include/SecurityUtils.h"
#include "../include/Logger.h"
#include <Windows.h>
#include <dpapi.h>
#include <vector>
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace UnnamedBot {

    SecurityUtils& SecurityUtils::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        static SecurityUtils* instance = new SecurityUtils();
        return *instance;
    }

    void SecurityUtils::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
        LOG_INFO("SecurityUtils", "Security utils initialized.");
    }

    std::string SecurityUtils::Encrypt(const std::string& plainText) {
        if (!initialized_) {
            LOG_ERROR("SecurityUtils", "Not initialized.");
            return plainText;
        }

        if (plainText.empty()) {
            return "";
        }

        std::vector<BYTE> inputData(plainText.begin(), plainText.end());
        auto encrypted = EncryptData(inputData);

        if (encrypted.empty()) {
            LOG_ERROR("SecurityUtils", "Encryption failed.");
            return plainText;
        }

        // 转换为Base64字符串以便存储
        DWORD base64Size = 0;
        if (!CryptBinaryToStringW(
            encrypted.data(),
            static_cast<DWORD>(encrypted.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &base64Size
        )) {
            LOG_ERROR("SecurityUtils", "Failed to get Base64 size.");
            return plainText;
        }

        std::vector<WCHAR> base64Buffer(base64Size);
        if (!CryptBinaryToStringW(
            encrypted.data(),
            static_cast<DWORD>(encrypted.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            base64Buffer.data(),
            &base64Size
        )) {
            LOG_ERROR("SecurityUtils", "Failed to convert to Base64.");
            return plainText;
        }

        std::wstring base64Str(base64Buffer.data());
        return std::string(base64Str.begin(), base64Str.end());
    }

    std::string SecurityUtils::Decrypt(const std::string& cipherText) {
        if (!initialized_) {
            LOG_ERROR("SecurityUtils", "Not initialized.");
            return cipherText;
        }

        if (cipherText.empty()) {
            return "";
        }

        // 从Base64解码
        std::wstring wideStr(cipherText.begin(), cipherText.end());
        DWORD binarySize = 0;
        if (!CryptStringToBinaryW(
            wideStr.c_str(),
            0,
            CRYPT_STRING_BASE64,
            nullptr,
            &binarySize,
            nullptr,
            nullptr
        )) {
            LOG_ERROR("SecurityUtils", "Failed to get binary size from Base64.");
            return cipherText;
        }

        std::vector<BYTE> encryptedData(binarySize);
        if (!CryptStringToBinaryW(
            wideStr.c_str(),
            0,
            CRYPT_STRING_BASE64,
            encryptedData.data(),
            &binarySize,
            nullptr,
            nullptr
        )) {
            LOG_ERROR("SecurityUtils", "Failed to decode Base64.");
            return cipherText;
        }

        auto decrypted = DecryptData(encryptedData);
        if (decrypted.empty()) {
            LOG_ERROR("SecurityUtils", "Decryption failed.");
            return cipherText;
        }

        return std::string(decrypted.begin(), decrypted.end());
    }

    std::vector<BYTE> SecurityUtils::EncryptData(const std::vector<BYTE>& data) {
        return CryptData(data, true);
    }

    std::vector<BYTE> SecurityUtils::DecryptData(const std::vector<BYTE>& data) {
        return CryptData(data, false);
    }

    std::vector<BYTE> SecurityUtils::CryptData(
        const std::vector<BYTE>& data,
        bool encrypt,
        const std::string& optionalEntropy
    ) {
        std::vector<BYTE> result;

        if (data.empty()) {
            return result;
        }

        DATA_BLOB inputBlob;
        inputBlob.pbData = const_cast<BYTE*>(data.data());
        inputBlob.cbData = static_cast<DWORD>(data.size());

        DATA_BLOB entropyBlob = {};
        DATA_BLOB* pEntropy = nullptr;

        if (!optionalEntropy.empty()) {
            entropyBlob.pbData = const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(optionalEntropy.c_str())
                );
            entropyBlob.cbData = static_cast<DWORD>(optionalEntropy.size());
            pEntropy = &entropyBlob;
        }

        DATA_BLOB outputBlob = {};

        BOOL resultBool = FALSE;
        if (encrypt) {
            resultBool = CryptProtectData(
                &inputBlob,
                nullptr,
                pEntropy,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &outputBlob
            );
        }
        else {
            resultBool = CryptUnprotectData(
                &inputBlob,
                nullptr,
                pEntropy,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &outputBlob
            );
        }

        if (!resultBool) {
            DWORD error = GetLastError();
            LOG_ERROR("SecurityUtils", "CryptData failed. Error: " + std::to_string(error));
            return result;
        }

        result.assign(outputBlob.pbData, outputBlob.pbData + outputBlob.cbData);
        LocalFree(outputBlob.pbData);

        return result;
    }

} // namespace UnnamedBot