#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace UnnamedBot {
	namespace Utils {

		// 字符串工具
		std::string ToLower(const std::string& str);
		std::string ToUpper(const std::string& str);
		bool StartsWith(const std::string& str, const std::string& prefix);
		bool EndsWith(const std::string& str, const std::string& suffix);
		std::string Trim(const std::string& str);
		std::vector<std::string> Split(const std::string& str, char delimiter);
		std::string Join(const std::vector<std::string>& parts, const std::string& delimiter);

		// 时间工具
		std::string FormatTimestamp(int64_t timestamp, const std::string& format = "%Y-%m-%d %H:%M:%S");
		int64_t GetCurrentTimestamp();
		std::string GetCurrentTimeString();

		// 文件工具
		bool ReadFileToString(const std::wstring& path, std::string& content);
		bool WriteStringToFile(const std::wstring& path, const std::string& content);
		bool FileExists(const std::wstring& path);
		int64_t GetFileSize(const std::wstring& path);

		// JSON工具
		std::string PrettyJson(const std::string& jsonStr);

		// 编码工具
		std::string Base64Encode(const std::vector<uint8_t>& data);
		std::vector<uint8_t> Base64Decode(const std::string& base64);
		std::string UrlEncode(const std::string& str);
		std::string UrlDecode(const std::string& str);

		// 系统工具
		std::string GetOSVersion();
		std::string GetComputerName();
		std::string GetUserName();

	} // namespace Utils
} // namespace UnnamedBot