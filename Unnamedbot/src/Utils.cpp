#include "../include/Utils.h"
#include "../include/Logger.h"
#include <Windows.h>
#include <wincodec.h>       // EncodeJpegBgra：WIC 的 JPEG 编码器
#include <fstream>
#include <filesystem>
#include <regex>
#include <codecvt>
#include <locale>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace UnnamedBot {
    namespace Utils {

        // ============ 字符串工具 ============

        std::string ToLower(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            return result;
        }

        std::string ToUpper(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(::toupper(c)); });
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

        std::filesystem::path ExecutableDirectory() {
            wchar_t buffer[MAX_PATH] = {};
            const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) {
                std::error_code ec;
                return std::filesystem::current_path(ec);
            }
            return std::filesystem::path(buffer).parent_path();
        }

        std::filesystem::path ResolveAppRelative(const std::wstring& relative) {
            std::error_code ec;
            const std::filesystem::path inWorkingDir = std::filesystem::path(relative);
            if (std::filesystem::exists(inWorkingDir, ec)) {
                return inWorkingDir;
            }
            return ExecutableDirectory() / relative;
        }

        std::vector<uint8_t> EncodeJpegBgra(const uint8_t* pixels, int width, int height, int stride, int quality, int maxWidth) {
            if (!pixels || width <= 0 || height <= 0) {
                return {};
            }
            if (stride <= 0) {
                stride = width * 4;
            }

            // WIC：BGRA8 源位图 → 24bppBGR 转换器 → JPEG 编码器 → 内存流。
            // 用内存流（CreateStreamOnHGlobal）取结果，不落盘。
            IWICImagingFactory* factory = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (FAILED(hr) || !factory) {
                LOG_ERROR("Utils", "WIC factory creation failed; JPEG encoding unavailable.");
                return {};
            }

            // 屏幕截图降采样：视觉模型自己也会缩，先缩掉能省下大部分上传体积
            // （4K 全屏原样 JPEG 轻松上 MB，缩到 1280 宽通常只剩一百多 KB）。
            std::vector<uint8_t> jpeg;
            IStream* stream = nullptr;
            IWICBitmapScaler* scaler = nullptr;
            IWICBitmap* scaled = nullptr;
            int outWidth = width;
            int outHeight = height;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IWICBitmap* source = nullptr;
            IWICFormatConverter* converter = nullptr;
            IPropertyBag2* props = nullptr;

            do {
                const UINT bufferSize = static_cast<UINT>(stride) * static_cast<UINT>(height);
                if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
                    break;
                }
                if (FAILED(factory->CreateBitmapFromMemory(
                    static_cast<UINT>(width), static_cast<UINT>(height),
                    GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(stride), bufferSize,
                    const_cast<BYTE*>(pixels), &source))) {
                    break;
                }
                if (maxWidth > 0 && width > maxWidth) {
                    const UINT w = static_cast<UINT>(maxWidth);
                    const UINT h = static_cast<UINT>((static_cast<long long>(height) * maxWidth) / width);
                    if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler) {
                        break;
                    }
                    if (FAILED(scaler->Initialize(source, w, h, WICBitmapInterpolationModeFant)) ||
                        FAILED(scaler->QueryInterface(IID_PPV_ARGS(&scaled)))) {
                        break;
                    }
                    outWidth = static_cast<int>(w);
                    outHeight = static_cast<int>(h);
                }
                IWICBitmapSource* src = scaled ? static_cast<IWICBitmapSource*>(scaled)
                                               : static_cast<IWICBitmapSource*>(source);
                if (FAILED(factory->CreateFormatConverter(&converter))) {
                    break;
                }
                // JPEG 编码器吃 24bppBGR，显式转一次，别让它自己挑格式
                if (FAILED(converter->Initialize(src, GUID_WICPixelFormat24bppBGR,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    break;
                }
                if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) {
                    break;
                }
                if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
                    break;
                }
                if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
                    break;
                }
                if (FAILED(frame->Initialize(props))) {
                    break;
                }

                // 质量：WIC 的 JPEG 用 ImageQuality（0.0~1.0）。
                // IPropertyBag2::Write 的形状是 Write(属性个数, PROPBAG2[], VARIANT[])。
                if (props) {
                    PROPBAG2 option = {};
                    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                    VARIANT value = {};
                    value.vt = VT_R4;
                    value.fltVal = static_cast<float>(quality) / 100.0f;
                    props->Write(1, &option, &value);
                }

                WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
                if (FAILED(frame->SetSize(static_cast<UINT>(outWidth), static_cast<UINT>(outHeight)))) {
                    break;
                }
                if (FAILED(frame->SetPixelFormat(&format))) {
                    break;
                }
                if (FAILED(frame->WriteSource(converter, nullptr))) {
                    break;
                }
                if (FAILED(frame->Commit())) {
                    break;
                }
                if (FAILED(encoder->Commit())) {
                    break;
                }

                // 取结果：先问流长度，再读出来
                STATSTG stat = {};
                if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
                    break;
                }
                const ULONGLONG size = stat.cbSize.QuadPart;
                if (size == 0) {
                    break;
                }
                LARGE_INTEGER zero = {};
                stream->Seek(zero, STREAM_SEEK_SET, nullptr);

                jpeg.resize(static_cast<size_t>(size));
                ULONG read = 0;
                if (FAILED(stream->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read)) || read == 0) {
                    jpeg.clear();
                    break;
                }
                jpeg.resize(read);
            } while (false);

            if (props) props->Release();
            if (converter) converter->Release();
            if (source) source->Release();
            if (frame) frame->Release();
            if (encoder) encoder->Release();
            if (stream) stream->Release();
            factory->Release();
            return jpeg;
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
            result.reserve(str.size());

            auto hexValue = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };

            for (size_t i = 0; i < str.size(); ++i) {
                char c = str[i];
                if (c == '+') {
                    result += ' ';
                }
                else if (c == '%' && i + 2 < str.size()) {
                    int high = hexValue(str[i + 1]);
                    int low = hexValue(str[i + 2]);
                    if (high >= 0 && low >= 0) {
                        result += static_cast<char>((high << 4) | low);
                        i += 2;
                    }
                    else {
                        // 无效的百分号转义，按原样保留
                        result += c;
                    }
                }
                else {
                    result += c;
                }
            }

            return result;
        }

        // ============ 路径/文本编码转换 ============

        std::wstring ImportLive2DModel(const std::filesystem::path& source, bool sourceIsZip) {
            std::error_code ec;
            if (!std::filesystem::exists(source, ec)) {
                LOG_WARN("Utils", "Import model: source not found.");
                return L"";
            }

            const std::filesystem::path modelsRoot = ResolveAppRelative(L"models");
            std::filesystem::create_directories(modelsRoot, ec);
            const std::filesystem::path staging = modelsRoot / L".importing";
            std::filesystem::remove_all(staging, ec);
            std::filesystem::create_directories(staging, ec);

            if (sourceIsZip) {
                // 系统自带 tar.exe（Win10 1803+）能解 zip，省得为这一件事引入解压库
                std::wstring cmd = L"tar.exe -xf \"" + source.wstring() + L"\" -C \"" +
                    staging.wstring() + L"\"";
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::wstring mutableCmd = cmd;
                if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    LOG_WARN("Utils", "Import model: failed to launch tar.exe.");
                    std::filesystem::remove_all(staging, ec);
                    return L"";
                }
                WaitForSingleObject(pi.hProcess, 60000);
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (code != 0) {
                    LOG_WARN("Utils", "Import model: tar.exe exited with " + std::to_string(code));
                    std::filesystem::remove_all(staging, ec);
                    return L"";
                }
            }
            else {
                std::filesystem::copy(source, staging,
                    std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    LOG_WARN("Utils", "Import model: copy failed: " + ec.message());
                    std::filesystem::remove_all(staging, ec);
                    return L"";
                }
            }

            // 找 *.model3.json（可能被多套了一层目录）
            std::filesystem::path modelJson;
            for (auto it = std::filesystem::recursive_directory_iterator(staging, ec);
                !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
                const std::filesystem::path& candidate = it->path();
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::wstring fileName = candidate.filename().wstring();
                const std::wstring suffix = L".model3.json";
                if (fileName.size() > suffix.size() &&
                    fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    modelJson = candidate;
                    break;
                }
            }
            if (modelJson.empty()) {
                LOG_WARN("Utils", "Import model: no *.model3.json found.");
                std::filesystem::remove_all(staging, ec);
                return L"";
            }

            const std::wstring fileName = modelJson.filename().wstring();
            const std::wstring modelId = fileName.substr(0, fileName.size() - std::wstring(L".model3.json").size());
            // 不覆盖已有模型：同名就顺延编号（haru-2、haru-3…）。
            // 以前是 remove_all 直接删掉重装，导错一次就把已调好的模型目录清了。
            std::filesystem::path target = modelsRoot / modelId;
            {
                std::error_code existEc;
                int suffix = 1;
                while (std::filesystem::exists(target, existEc)) {
                    ++suffix;
                    target = modelsRoot / (modelId + L"-" + std::to_wstring(suffix));
                }
            }
            std::filesystem::path sourceDir = modelJson.parent_path();
            if (sourceDir == staging) {
                // 模型文件直接躺在压缩包根上：把 staging 整个改名过去
                std::filesystem::rename(staging, target, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy(staging, target,
                        std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
            else {
                std::filesystem::rename(sourceDir, target, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy(sourceDir, target,
                        std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
            std::filesystem::remove_all(staging, ec);

            const std::wstring finalId = target.filename().wstring();
            const std::filesystem::path expected = target / (finalId + L".model3.json");
            if (!std::filesystem::exists(expected, ec)) {
                LOG_WARN("Utils", "Import model: expected " + PathToUtf8(expected));
                return L"";
            }
            LOG_INFO("Utils", "Model imported as: " + PathToUtf8(finalId));
            return finalId;
        }

        std::string PathToUtf8(const std::filesystem::path& path) {
            const std::u8string utf8 = path.u8string();
            return std::string(utf8.begin(), utf8.end());
        }

        std::string WideToUtf8(const std::wstring& wstr) {
            if (wstr.empty()) return "";

            const int sizeNeeded = WideCharToMultiByte(
                CP_UTF8, 0,
                wstr.c_str(), static_cast<int>(wstr.size()),
                nullptr, 0,
                nullptr, nullptr
            );
            if (sizeNeeded <= 0) return "";

            std::string result(static_cast<size_t>(sizeNeeded), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0,
                wstr.c_str(), static_cast<int>(wstr.size()),
                &result[0], sizeNeeded,
                nullptr, nullptr
            );
            return result;
        }

        std::wstring Utf8ToWide(const std::string& str) {
            if (str.empty()) return L"";

            const int sizeNeeded = MultiByteToWideChar(
                CP_UTF8, 0,
                str.c_str(), static_cast<int>(str.size()),
                nullptr, 0
            );
            if (sizeNeeded <= 0) return L"";

            std::wstring result(static_cast<size_t>(sizeNeeded), L'\0');
            MultiByteToWideChar(
                CP_UTF8, 0,
                str.c_str(), static_cast<int>(str.size()),
                &result[0], sizeNeeded
            );
            return result;
        }

        // ============ 系统工具 ============

        std::string GetOSVersion() {
            using RtlGetVersionPtr = LONG(WINAPI*)(OSVERSIONINFOW*);

            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
                    GetProcAddress(ntdll, "RtlGetVersion"));
                if (rtlGetVersion) {
                    OSVERSIONINFOW osvi = {};
                    osvi.dwOSVersionInfoSize = sizeof(osvi);
                    if (rtlGetVersion(&osvi) == 0) {
                        return "Windows " + std::to_string(osvi.dwMajorVersion) + "." +
                            std::to_string(osvi.dwMinorVersion) + " (Build " +
                            std::to_string(osvi.dwBuildNumber) + ")";
                    }
                }
            }

            return "Windows";
        }

        std::string GetComputerName() {
            wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
            if (!::GetComputerNameW(buffer, &size)) {
                return "";
            }
            return WideToUtf8(std::wstring(buffer, size));
        }

        std::string GetUserName() {
            DWORD size = 0;
            // 首次调用仅用于获取所需缓冲区长度（返回 FALSE，错误码 ERROR_MORE_DATA）
            ::GetUserNameW(nullptr, &size);
            if (size == 0) {
                return "";
            }

            std::wstring buffer(size, L'\0');
            if (!::GetUserNameW(&buffer[0], &size)) {
                return "";
            }
            // 成功时 size 包含结尾的空字符
            buffer.resize(size > 0 ? size - 1 : 0);
            return WideToUtf8(buffer);
        }

    } // namespace Utils
} // namespace UnnamedBot