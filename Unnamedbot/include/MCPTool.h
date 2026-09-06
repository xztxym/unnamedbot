#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

    // MCP工具执行结果
    struct MCPToolResult {
        bool success = true;
        std::string content;           // 文本内容
        nlohmann::json data;           // 结构化数据
        std::string error;
        bool requiresConfirmation = false;  // 是否需要用户确认
        std::string confirmationMessage;
    };

    // 工具执行回调（用于需要用户确认的工具）
    using ConfirmationCallback = std::function<bool(const std::string& message)>;

    // MCP工具基类
    class MCPToolBase {
    public:
        virtual ~MCPToolBase() = default;
        virtual std::string GetName() const = 0;
        virtual std::string GetDescription() const = 0;
        virtual nlohmann::json GetParametersSchema() const = 0;
        virtual MCPToolResult Execute(const nlohmann::json& parameters) = 0;
        virtual bool RequiresConfirmation() const { return false; }
    };

    // MCP工具管理器
    class MCPToolManager {
    public:
        static MCPToolManager& Instance();

        // 注册工具
        void RegisterTool(std::shared_ptr<MCPToolBase> tool);

        // 注册所有默认工具
        void RegisterDefaultTools();

        // 获取所有工具定义（用于AI请求）
        std::vector<ToolDefinition> GetToolDefinitions() const;

        // 执行工具
        MCPToolResult ExecuteTool(const std::string& toolName, const nlohmann::json& parameters);

        // 执行工具（带确认回调）
        MCPToolResult ExecuteTool(
            const std::string& toolName,
            const nlohmann::json& parameters,
            ConfirmationCallback confirmCallback
        );

        // 获取工具列表
        std::vector<std::string> GetToolNames() const;

        // 检查工具是否存在
        bool HasTool(const std::string& toolName) const;

        // 设置确认回调（全局）
        void SetConfirmationCallback(ConfirmationCallback callback);

    private:
        MCPToolManager() = default;
        ~MCPToolManager() = default;
        MCPToolManager(const MCPToolManager&) = delete;
        MCPToolManager& operator=(const MCPToolManager&) = delete;

        // 工具映射
        std::map<std::string, std::shared_ptr<MCPToolBase>> tools_;
        ConfirmationCallback defaultConfirmCallback_;
        mutable std::mutex mutex_;
    };

    // ============ 具体工具实现 ============

    // 1. 获取当前时间
    class GetCurrentTimeTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "get_current_time"; }
        std::string GetDescription() const override {
            return "Get the current system time with timezone information.";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object();
        }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 2. 获取前台进程
    class GetFrontendProcessesTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "get_frontend_processes"; }
        std::string GetDescription() const override {
            return "Get a list of currently running frontend applications (with window titles and executable paths).";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object();
        }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 3. 获取天气
    class GetWeatherTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "get_weather"; }
        std::string GetDescription() const override {
            return "Get weather information for a specified city using AMap API.";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object({
                {"city", nlohmann::json::object({
                    {"type", "string"},
                    {"description", "City name in Chinese (e.g., '北京', '上海')"}
                })}
                });
        }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 4. 获取屏幕截图
    class GetScreenCaptureTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "get_screen_capture"; }
        std::string GetDescription() const override {
            return "Capture the current desktop screen and return as base64 image data.";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object();
        }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 5. 打开URL（需确认）
    class OpenUrlTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "open_url"; }
        std::string GetDescription() const override {
            return "Open a URL in the default browser. Requires user confirmation.";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object({
                {"url", nlohmann::json::object({
                    {"type", "string"},
                    {"description", "The URL to open"}
                })}
                });
        }
        bool RequiresConfirmation() const override { return true; }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 6. 发送通知（需确认）
    class SendNotificationTool : public MCPToolBase {
    public:
        std::string GetName() const override { return "send_notification"; }
        std::string GetDescription() const override {
            return "Send a system notification. Requires user confirmation.";
        }
        nlohmann::json GetParametersSchema() const override {
            return nlohmann::json::object({
                {"text", nlohmann::json::object({
                    {"type", "string"},
                    {"description", "The notification text to display"}
                })},
                {"title", nlohmann::json::object({
                    {"type", "string"},
                    {"description", "Optional notification title"}
                })}
                });
        }
        bool RequiresConfirmation() const override { return true; }
        MCPToolResult Execute(const nlohmann::json& parameters) override;
    };

    // 7. 预留：查询日历
    // 8. 预留：控制智能家居

    // 工具辅助函数
    namespace MCPUtils {
        // 获取城市天气（高德API）
        std::string GetWeatherByCity(const std::string& city, const std::string& apiKey);

        // 获取IP地址
        std::string GetPublicIP();

        // 获取IP所在城市
        std::string GetCityByIP(const std::string& ip);

        // 计算两个经纬度之间的距离（公里）
        double CalculateDistance(double lat1, double lon1, double lat2, double lon2);

        // 屏幕截图（DXGI）
        bool CaptureScreen(std::vector<unsigned char>& imageData, int& width, int& height);

        // 获取前台进程列表
        std::vector<std::map<std::string, std::string>> GetFrontendProcesses();
    }

} // namespace UnnamedBot