#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <filesystem>

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

		// 把 BGRA8 原始像素编成 JPEG（WIC）。
		// 截屏拿到的是裸像素，直接 base64 塞进请求既超限也不是模型认的图片格式；
		// JPEG 质量 80 的 1920x1080 大约是 250 KB，base64 后约 340 KB，远低于 32 MiB 上限。
		// 调用线程需要已初始化 COM（内部用 WIC 工厂）。
		std::vector<uint8_t> EncodeJpegBgra(const uint8_t* pixels, int width, int height, int stride, int quality = 80, int maxWidth = 0);

		// exe 所在目录。安装之后从快捷方式、任务栏跳转列表、文件关联启动时，
		// 工作目录并不等于 exe 目录，"data/logs/models/prompts" 这类相对路径
		// 必须有一个与工作目录无关的基准。
		std::filesystem::path ExecutableDirectory();

		// 相对路径的落地位置：工作目录下已经存在就沿用它（开发时的布局，
		// 直接跑 x64\Release 时仍在工程目录里读写），否则落到 exe 目录旁边。
		std::filesystem::path ResolveAppRelative(const std::wstring& relative);

		// 路径/文本编码转换
		// C++20 下 path::u8string() 返回 std::u8string，不能隐式转换为 std::string，
		// 因此统一通过该函数获取 UTF-8 字节串（sqlite3、日志、HTTP 均需要 UTF-8）。
		std::string PathToUtf8(const std::filesystem::path& path);

        // 把一个 Live2D 模型导入到 models/ 目录。
        // source 可以是 .zip（用系统自带 tar.exe 解压，免第三方库）或文件夹。
        // 会在结果里找 *.model3.json，按它的文件名建目录（扫描器要求
        // <目录名>/<目录名>.model3.json），成功返回目录名，失败返回空串。
        std::wstring ImportLive2DModel(const std::filesystem::path& source, bool sourceIsZip);
		std::string WideToUtf8(const std::wstring& wstr);
		std::wstring Utf8ToWide(const std::string& str);

		// 系统工具
		std::string GetOSVersion();
		std::string GetComputerName();
		std::string GetUserName();

	} // namespace Utils
} // namespace UnnamedBot