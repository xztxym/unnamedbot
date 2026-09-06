#include "../include/Utils.h"
#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <codecvt>
#include <locale>

namespace UnnamedBot {
    namespace Utils {

        // ============ 字符串工具 ============

        std::string ToLower(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(), ::tolower);
            return result;
        }

        std::string ToUpper(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(), ::toupper);
            return result;
        }

        bool StartsWith(const std::string& str, const std::string& prefix) {
            return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
        }

        bool EndsWith(const std::string& str, const std::string& suffix) {
            return str.size() >= suffix.size() &&
                str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        std::string Trim(const std::string& str) {
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return "";
            size_t end = str.find_last_not_of(" \t\n\r");
            return str.substr(start, end - start + 1);
        }

        std::vector<std::string> Split(const std::string& str, char delimiter) {
            std::vector<std::string> result;
            std::istringstream iss(str);
            std::string token;
            while (std::getline(iss, token, delimiter)) {
                result.push_back(token);
            }
            return result;
        }

        std::string Join(const std::vector<std::string>& parts, const std::string& delimiter) {
            std::ostringstream oss;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) oss << delimiter;
                oss << parts[i];
            }
            return oss.str();
        }

        // ============ 时间工具 ============

        std::string FormatTimestamp(int64_t timestamp, const std::string& format) {
            time_t time = static_cast<time_t>(timestamp);
            struct tm tm_time;
            localtime_s(&tm_time, &time);
            char buffer[256];
            strftime(buffer, sizeof(buffer), format.c_str(), &tm_time);
            return std::string(buffer);
        }

        int64_t GetCurrentTimestamp() {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        }

        std::string GetCurrentTimeString() {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm tm_now;
            localtime_s(&tm_now, &time_t_now);
            std::ostringstream oss;
            oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
            return oss.str();
        }

        // ============ 文件工具 ============

        bool ReadFileToString(const std::wstring& path, std::string& content) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return false;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            file.seekg(0, std::ios::end);
            size_t size = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);

            content.resize(size);
            file.read(&content[0], size);
            file.close();
            return true;
        }

        bool WriteStringToFile(const std::wstring& path, const std::string& content) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            if (ec) return false;

            std::ofstream file(path, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            file.write(content.c_str(), content.size());
            file.close();
            return true;
        }

        bool FileExists(const std::wstring& path) {
            std::error_code ec;
            return std::filesystem::exists(path, ec);
        }

        int64_t GetFileSize(const std::wstring& path) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return -1;
            }
            return static_cast<int64_t>(std::filesystem::file_size(path, ec));
        }

        // ============ JSON工具 ============

        std::string PrettyJson(const std::string& jsonStr) {
            try {
                auto json = nlohmann::json::parse(jsonStr);
                return json.dump(2);
            }
            catch (...) {
                return jsonStr;
            }
        }

        // ============ 编码工具 ============

        std::string Base64Encode(const std::vector<uint8_t>& data) {
            if (data.empty()) return "";

            DWORD size = 0;
            CryptBinaryToStringA(
                data.data(),
                static_cast<DWORD>(data.size()),
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                nullptr,
                &size
            );

            std::string result(size, '\0');
            CryptBinaryToStringA(
                data.data(),
                static_cast<DWORD>(data.size()),
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                &result[0],
                &size
            );

            // 移除末尾的空字符
            while (!result.empty() && result.back() == '\0') {
                result.pop_back();
            }
            return result;
        }

        std::vector<uint8_t> Base64Decode(const std::string& base64) {
            if (base64.empty()) return {};

            DWORD size = 0;
            CryptStringToBinaryA(
                base64.c_str(),
                0,
                CRYPT_STRING_BASE64,
                nullptr,
                &size,
                nullptr,
                nullptr
            );

            std::vector<uint8_t> result(size);
            CryptStringToBinaryA(
                base64.c_str(),
                0,
                CRYPT_STRING_BASE64,
                result.data(),
                &size,
                nullptr,
                nullptr
            );

            return result;
        }

        std::string UrlEncode(const std::string& str) {
            std::ostringstream escaped;
            escaped.fill('0');
            escaped << std::hex;

            for (char c : str) {
                if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                    escaped << c;
                }
                else {
                    escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
                }
            }

            return escaped.str();
        }

        std::string UrlDecode(const std::string& str) {
            std::string result;
            for (size_t i