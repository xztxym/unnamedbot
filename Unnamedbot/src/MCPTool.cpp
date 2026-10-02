#include "../include/MCPTool.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <Windows.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace UnnamedBot {

    // ============ MCPToolManager 实现 ============

    MCPToolManager& MCPToolManager::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        static MCPToolManager* instance = new MCPToolManager();
        return *instance;
    }

    void MCPToolManager::RegisterTool(std::shared_ptr<MCPToolBase> tool) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!tool) return;
        std::string name = tool->GetName();
        tools_[name] = tool;
        LOG_DEBUG("MCPToolManager", "Registered tool: " + name);
    }

    void MCPToolManager::RegisterDefaultTools() {
        RegisterTool(std::make_shared<GetCurrentTimeTool>());
        RegisterTool(std::make_shared<GetFrontendProcessesTool>());
        RegisterTool(std::make_shared<GetWeatherTool>());
        RegisterTool(std::make_shared<GetScreenCaptureTool>());
        RegisterTool(std::make_shared<OpenUrlTool>());
        RegisterTool(std::make_shared<SendNotificationTool>());
        LOG_INFO("MCPToolManager", "Registered default tools.");
    }

    std::vector<ToolDefinition> MCPToolManager::GetToolDefinitions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ToolDefinition> definitions;

        for (const auto& pair : tools_) {
            const auto& tool = pair.second;
            ToolDefinition def;
            def.type = "function";
            def.function.name = tool->GetName();
            def.function.description = tool->GetDescription();

            // GetParametersSchema() 给的是「属性表」（参数名 -> 参数描述），
            // 而 ToolDefinition::function.parameters 按约定要的是完整 JSON Schema。
            // 少了这层包裹，参数为空的工具会送出 "parameters": {}，服务端一律拒成
            // HTTP 400：Invalid schema for function 'get_current_time':
            // schema must be a JSON Schema of 'type: "object"', got 'type: null'。
            // 结果是「只要请求带上工具就必然失败」——AI 一直不回话的元凶之一。
            const nlohmann::json properties = tool->GetParametersSchema();
            def.function.parameters = {
                { "type", "object" },
                { "properties", properties.is_object() ? properties : nlohmann::json::object() },
            };
            definitions.push_back(def);
        }

        return definitions;
    }

    MCPToolResult MCPToolManager::ExecuteTool(const std::string& toolName, const nlohmann::json& parameters) {
        // 使用默认回调（如果有）
        return ExecuteTool(toolName, parameters, defaultConfirmCallback_);
    }

    MCPToolResult MCPToolManager::ExecuteTool(
        const std::string& toolName,
        const nlohmann::json& parameters,
        ConfirmationCallback confirmCallback
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tools_.find(toolName);
        if (it == tools_.end()) {
            MCPToolResult result;
            result.success = false;
            result.error = "Tool not found: " + toolName;
            LOG_ERROR("MCPToolManager", result.error);
            return result;
        }

        auto& tool = it->second;

        // 检查是否需要确认
        if (tool->RequiresConfirmation()) {
            if (!confirmCallback) {
                // 没有确认回调，直接返回需要确认状态
                MCPToolResult result;
                result.success = false;
                result.requiresConfirmation = true;
                result.confirmationMessage = "Tool " + toolName + " requires user confirmation.";
                return result;
            }

            // 显示确认对话框
            std::string confirmMsg = "Do you want to execute tool: " + toolName + "?\n\n";
            confirmMsg += "Parameters: " + parameters.dump(2);

            if (!confirmCallback(confirmMsg)) {
                MCPToolResult result;
                result.success = false;
                result.error = "User cancelled tool execution.";
                LOG_INFO("MCPToolManager", "Tool execution cancelled by user: " + toolName);
                return result;
            }
        }

        LOG_DEBUG("MCPToolManager", "Executing tool: " + toolName + " with params: " + parameters.dump());
        return tool->Execute(parameters);
    }

    std::vector<std::string> MCPToolManager::GetToolNames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& pair : tools_) {
            names.push_back(pair.first);
        }
        return names;
    }

    bool MCPToolManager::HasTool(const std::string& toolName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tools_.find(toolName) != tools_.end();
    }

    void MCPToolManager::SetConfirmationCallback(ConfirmationCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultConfirmCallback_ = callback;
    }

    // ============ GetCurrentTimeTool 实现 ============

    MCPToolResult GetCurrentTimeTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm tm_now;
            localtime_s(&tm_now, &time_t_now);

            std::ostringstream oss;
            oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");

            // 获取时区信息
            TIME_ZONE_INFORMATION tzInfo;
            GetTimeZoneInformation(&tzInfo);
            int biasMinutes = tzInfo.Bias;
            char tzSign = biasMinutes <= 0 ? '+' : '-';
            int tzHours = abs(biasMinutes) / 60;
            int tzMins = abs(biasMinutes) % 60;

            oss << " (UTC" << tzSign << std::setw(2) << std::setfill('0') << tzHours
                << ":" << std::setw(2) << std::setfill('0') << tzMins << ")";

            result.content = oss.str();
            result.data["datetime"] = oss.str();
            result.data["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();

            LOG_DEBUG("GetCurrentTimeTool", "Current time: " + result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get time: " + std::string(e.what());
            LOG_ERROR("GetCurrentTimeTool", result.error);
        }

        return result;
    }

    // ============ GetFrontendProcessesTool 实现 ============

    MCPToolResult GetFrontendProcessesTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            auto processes = MCPUtils::GetFrontendProcesses();
            nlohmann::json processArray = nlohmann::json::array();

            for (const auto& proc : processes) {
                nlohmann::json procJson;
                procJson["name"] = proc.at("name");
                procJson["title"] = proc.at("title");
                procJson["path"] = proc.at("path");
                procJson["pid"] = std::stoi(proc.at("pid"));
                processArray.push_back(procJson);
            }

            result.data["processes"] = processArray;
            result.data["count"] = processArray.size();

            // 生成文本描述
            std::ostringstream oss;
            oss << "Found " << processArray.size() << " frontend applications:\n";
            for (const auto& proc : processes) {
                oss << "  - " << proc.at("title") << " (" << proc.at("name") << ")\n";
            }
            result.content = oss.str();

            LOG_DEBUG("GetFrontendProcessesTool", "Retrieved " + std::to_string(processes.size()) + " processes.");
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get processes: " + std::string(e.what());
            LOG_ERROR("GetFrontendProcessesTool", result.error);
        }

        return result;
    }

    // ============ GetWeatherTool 实现 ============

    MCPToolResult GetWeatherTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            // 获取城市参数
            std::string city;
            if (parameters.contains("city") && parameters["city"].is_string()) {
                city = parameters["city"];
            }
            else {
                // 默认使用IP获取城市
                std::string ip = MCPUtils::GetPublicIP();
                city = MCPUtils::GetCityByIP(ip);
                if (city.empty()) {
                    city = "北京"; // 默认
                }
            }

            // 获取高德API Key
            std::string apiKey = ConfigManager::Instance().GetString(ConfigManager::KEY_AMAP_API_KEY);
            if (apiKey.empty()) {
                result.success = false;
                result.error = "AMap API Key not configured. Please set it in settings.";
                LOG_ERROR("GetWeatherTool", result.error);
                return result;
            }

            std::string weatherData = MCPUtils::GetWeatherByCity(city, apiKey);
            if (weatherData.empty()) {
                result.success = false;
                result.error = "Failed to get weather data for city: " + city;
                LOG_ERROR("GetWeatherTool", result.error);
                return result;
            }

            auto jsonData = nlohmann::json::parse(weatherData);
            if (jsonData.contains("status") && jsonData["status"] == "1") {
                result.data = jsonData;
                result.content = "Weather for " + city + ":\n";

                if (jsonData.contains("lives") && jsonData["lives"].is_array() && !jsonData["lives"].empty()) {
                    auto live = jsonData["lives"][0];
                    result.content += "  Weather: " + live["weather"].get<std::string>() + "\n";
                    result.content += "  Temperature: " + live["temperature"].get<std::string>() + "°C\n";
                    result.content += "  Wind: " + live["winddirection"].get<std::string>() + " " + live["windpower"].get<std::string>() + "\n";
                    result.content += "  Humidity: " + live["humidity"].get<std::string>() + "%\n";
                    result.content += "  Report Time: " + live["reporttime"].get<std::string>();
                }
            }
            else {
                result.success = false;
                result.error = "API returned error: " + jsonData.dump();
                LOG_ERROR("GetWeatherTool", result.error);
            }
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get weather: " + std::string(e.what());
            LOG_ERROR("GetWeatherTool", result.error);
        }

        return result;
    }

    // ============ GetScreenCaptureTool 实现 ============

    MCPToolResult GetScreenCaptureTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            std::vector<unsigned char> imageData;
            int width = 0, height = 0;

            if (!MCPUtils::CaptureScreen(imageData, width, height)) {
                result.success = false;
                result.error = "Failed to capture screen.";
                LOG_ERROR("GetScreenCaptureTool", result.error);
                return result;
            }

            // 将图像数据转换为Base64
            std::string base64Data;
            // 这里简化处理，实际应该使用Base64编码
            // 暂时返回占位符
            result.data["width"] = width;
            result.data["height"] = height;
            result.data["size"] = imageData.size();
            result.data["base64"] = "BASE64_DATA_PLACEHOLDER"; // 实际需要Base64编码
            result.data["format"] = "png";

            result.content = "Screen captured successfully. Size: " + std::to_string(width) + "x" + std::to_string(height);
            LOG_DEBUG("GetScreenCaptureTool", result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to capture screen: " + std::string(e.what());
            LOG_ERROR("GetScreenCaptureTool", result.error);
        }

        return result;
    }

    // ============ OpenUrlTool 实现 ============

    MCPToolResult OpenUrlTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            if (!parameters.contains("url") || !parameters["url"].is_string()) {
                result.success = false;
                result.error = "Missing 'url' parameter.";
                LOG_ERROR("OpenUrlTool", result.error);
                return result;
            }

            std::string url = parameters["url"];

            // 打开URL
            SHELLEXECUTEINFO sei = {};
            sei.cbSize = sizeof(SHELLEXECUTEINFO);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpFile = url.c_str();
            sei.nShow = SW_SHOWNORMAL;

            if (ShellExecuteEx(&sei)) {
                if (sei.hProcess) {
                    CloseHandle(sei.hProcess);
                }
                result.content = "Opened URL: " + url;
                result.data["url"] = url;
                LOG_DEBUG("OpenUrlTool", result.content);
            }
            else {
                result.success = false;
                result.error = "Failed to open URL: " + url + " (Error: " + std::to_string(GetLastError()) + ")";
                LOG_ERROR("OpenUrlTool", result.error);
            }
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to open URL: " + std::string(e.what());
            LOG_ERROR("OpenUrlTool", result.error);
        }

        return result;
    }

    // ============ SendNotificationTool 实现 ============

    MCPToolResult SendNotificationTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            if (!parameters.contains("text") || !parameters["text"].is_string()) {
                result.success = false;
                result.error = "Missing 'text' parameter.";
                LOG_ERROR("SendNotificationTool", result.error);
                return result;
            }

            std::string text = parameters["text"];
            std::string title = parameters.contains("title") && parameters["title"].is_string()
                ? parameters["title"].get<std::string>()
                : "UnnamedBot Notification";

            // 使用Windows通知（系统托盘气球提示）
            // 这里简化，使用MessageBox作为替代
            // 实际应使用托盘通知
            MessageBoxW(nullptr,
                std::wstring(text.begin(), text.end()).c_str(),
                std::wstring(title.begin(), title.end()).c_str(),
                MB_OK | MB_ICONINFORMATION);

            result.content = "Notification sent: " + text;
            result.data["title"] = title;
            result.data["text"] = text;
            LOG_DEBUG("SendNotificationTool", result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to send notification: " + std::string(e.what());
            LOG_ERROR("SendNotificationTool", result.error);
        }

        return result;
    }

    // ============ MCPUtils 实现 ============

    namespace MCPUtils {

        std::string GetWeatherByCity(const std::string& city, const std::string& apiKey) {
            try {
                // 首先根据城市名获取城市编码
                std::string geoUrl = "https://restapi.amap.com/v3/geocode/geo";
                std::string geoParams = "?address=" + city + "&key=" + apiKey;

                auto geoResponse = cpr::Get(
                    cpr::Url{ geoUrl + geoParams },
                    cpr::Timeout{ std::chrono::seconds(10) }
                );

                if (geoResponse.status_code != 200) {
                    LOG_ERROR("MCPUtils", "Failed to get city code: " + geoResponse.text);
                    return "";
                }

                auto geoJson = nlohmann::json::parse(geoResponse.text);
                if (geoJson.contains("status") && geoJson["status"] == "1" &&
                    geoJson.contains("geocodes") && geoJson["geocodes"].is_array() &&
                    !geoJson["geocodes"].empty()) {

                    std::string cityCode = geoJson["geocodes"][0]["adcode"];

                    // 获取天气
                    std::string weatherUrl = "https://restapi.amap.com/v3/weather/weatherInfo";
                    std::string weatherParams = "?city=" + cityCode + "&key=" + apiKey + "&extensions=base";

                    auto weatherResponse = cpr::Get(
                        cpr::Url{ weatherUrl + weatherParams },
                        cpr::Timeout{ std::chrono::seconds(10) }
                    );

                    if (weatherResponse.status_code == 200) {
                        return weatherResponse.text;
                    }
                    else {
                        LOG_ERROR("MCPUtils", "Failed to get weather: " + weatherResponse.text);
                    }
                }
                else {
                    LOG_ERROR("MCPUtils", "Failed to get city code: " + geoResponse.text);
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetWeatherByCity exception: " + std::string(e.what()));
            }

            return "";
        }

        std::string GetPublicIP() {
            try {
                auto response = cpr::Get(
                    cpr::Url{ "http://ip-api.com/json/" },
                    cpr::Timeout{ std::chrono::seconds(5) }
                );

                if (response.status_code == 200) {
                    auto json = nlohmann::json::parse(response.text);
                    if (json.contains("query")) {
                        return json["query"];
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetPublicIP exception: " + std::string(e.what()));
            }
            return "";
        }

        std::string GetCityByIP(const std::string& ip) {
            try {
                std::string url = "http://ip-api.com/json/" + ip;
                auto response = cpr::Get(
                    cpr::Url{ url },
                    cpr::Timeout{ std::chrono::seconds(5) }
                );

                if (response.status_code == 200) {
                    auto json = nlohmann::json::parse(response.text);
                    if (json.contains("city")) {
                        return json["city"];
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetCityByIP exception: " + std::string(e.what()));
            }
            return "";
        }

        double CalculateDistance(double lat1, double lon1, double lat2, double lon2) {
            // 使用Haversine公式计算距离（公里）
            const double R = 6371.0; // 地球半径（公里）
            double dLat = (lat2 - lat1) * 3.141592653589793 / 180.0;
            double dLon = (lon2 - lon1) * 3.141592653589793 / 180.0;
            double a = sin(dLat / 2) * sin(dLat / 2) +
                cos(lat1 * 3.141592653589793 / 180.0) * cos(lat2 * 3.141592653589793 / 180.0) *
                sin(dLon / 2) * sin(dLon / 2);
            double c = 2 * atan2(sqrt(a), sqrt(1 - a));
            return R * c;
        }

        bool CaptureScreen(std::vector<unsigned char>& imageData, int& width, int& height) {
            // 使用DXGI捕获屏幕
            HRESULT hr = S_OK;

            // 创建DXGI设备
            IDXGIFactory1* factory = nullptr;
            hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
            if (FAILED(hr)) {
                LOG_ERROR("MCPUtils", "Failed to create DXGI factory.");
                return false;
            }

            // 获取第一个适配器
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(0, &adapter);
            if (FAILED(hr)) {
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to enumerate adapters.");
                return false;
            }

            // 创建D3D11设备
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            D3D_FEATURE_LEVEL level;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
            if (FAILED(hr)) {
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to create D3D11 device.");
                return false;
            }

            // 获取输出（DuplicateOutput 定义在 IDXGIOutput1 上）
            IDXGIOutput* outputBase = nullptr;
            hr = adapter->EnumOutputs(0, &outputBase);
            if (FAILED(hr)) {
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to enumerate outputs.");
                return false;
            }

            IDXGIOutput1* output = nullptr;
            hr = outputBase->QueryInterface(IID_PPV_ARGS(&output));
            outputBase->Release();
            if (FAILED(hr)) {
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to query IDXGIOutput1.");
                return false;
            }

            // 获取桌面大小
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            // 创建输出复制
            IDXGIOutputDuplication* duplication = nullptr;
            hr = output->DuplicateOutput(device, &duplication);
            if (FAILED(hr)) {
                output->Release();
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to duplicate output.");
                return false;
            }

            // 获取桌面帧
            IDXGIResource* resource = nullptr;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            hr = duplication->AcquireNextFrame(1000, &frameInfo, &resource);
            if (SUCCEEDED(hr) && resource) {
                // 获取纹理
                ID3D11Texture2D* texture = nullptr;
                hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
                if (SUCCEEDED(hr) && texture) {
                    // 获取纹理描述
                    D3D11_TEXTURE2D_DESC texDesc;
                    texture->GetDesc(&texDesc);

                    // 创建CPU可访问的纹理
                    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDesc.MiscFlags = 0;

                    ID3D11Texture2D* stagingTexture = nullptr;
                    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
                    if (SUCCEEDED(hr)) {
                        // 复制到CPU可访问纹理
                        context->CopyResource(stagingTexture, texture);

                        // 映射数据
                        D3D11_MAPPED_SUBRESOURCE mapped;
                        hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
                        if (SUCCEEDED(hr)) {
                            // 计算图像数据大小（简化为PNG格式，但这里只保存原始数据）
                            size_t dataSize = mapped.RowPitch * texDesc.Height;
                            imageData.resize(dataSize);
                            memcpy(imageData.data(), mapped.pData, dataSize);
                            context->Unmap(stagingTexture, 0);
                        }
                        stagingTexture->Release();
                    }
                    texture->Release();
                }
                resource->Release();
                duplication->ReleaseFrame();
            }

            // 释放资源
            duplication->Release();
            output->Release();
            device->Release();
            context->Release();
            adapter->Release();
            factory->Release();

            return !imageData.empty();
        }

        std::vector<std::map<std::string, std::string>> GetFrontendProcesses() {
            std::vector<std::map<std::string, std::string>> processes;

            // 获取所有窗口
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                auto* processesPtr = reinterpret_cast<std::vector<std::map<std::string, std::string>>*>(lParam);

                // 检查窗口是否可见且不是子窗口
                if (!IsWindowVisible(hwnd)) return TRUE;
                if (GetParent(hwnd) != nullptr) return TRUE;
                if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

                // 获取窗口标题
                wchar_t windowTitle[256];
                GetWindowTextW(hwnd, windowTitle, 256);
                std::wstring titleW = windowTitle;
                if (titleW.empty()) return TRUE;

                // 获取进程ID
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == 0) return TRUE;

                // 获取进程句柄
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                if (!hProcess) return TRUE;

                // 获取进程可执行文件名
                wchar_t exePath[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
                    std::wstring exePathW = exePath;
                    std::string exeName = Utils::PathToUtf8(std::filesystem::path(exePathW).filename());

                    std::map<std::string, std::string> proc;
                    proc["title"] = Utils::WideToUtf8(titleW);
                    proc["name"] = exeName;
                    proc["path"] = Utils::PathToUtf8(std::filesystem::path(exePathW));
                    proc["pid"] = std::to_string(pid);
                    processesPtr->push_back(proc);
                }

                CloseHandle(hProcess);
                return TRUE;
                }, reinterpret_cast<LPARAM>(&processes));

            return processes;
        }

        // 辅助：wstring转string
        std::string wstring_to_utf8(const std::wstring& wstr) {
            if (wstr.empty()) return "";
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string str(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, nullptr, nullptr);
            return str;
        }

    } // namespace MCPUtils

} // namespace UnnamedBot