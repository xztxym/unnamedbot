#include "../include/AppCore.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Database.h"
#include "../include/Window.h"
#include "../include/Renderer.h"
#include "../include/Live2DManager.h"
#include "../include/AIClient.h"
#include "../include/MCPTool.h"
#include "../include/MessageHandler.h"
#include "../include/TrayIcon.h"
#include "../include/HotkeyManager.h"
#include "../include/SecurityUtils.h"
#include "../include/AttachmentManager.h"
#include "../include/TaskScheduler.h"
#include "../include/UpdateManager.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <shellapi.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace UnnamedBot {

    const wchar_t* AppCore::WINDOW_CLASS_NAME = L"UnnamedBotAppClass";
    const UINT AppCore::WM_TRAYICON = WM_USER + 100;
    const UINT AppCore::WM_HOTKEY = WM_USER + 101;

    AppCore& AppCore::Instance() {
        static AppCore instance;
        return instance;
    }

    bool AppCore::Initialize(HINSTANCE hInstance, int nCmdShow) {
        if (isRunning_) {
            LOG_WARN("AppCore", "Application already initialized.");
            return true;
        }

        instance_ = hInstance;
        isRunning_ = true;
        isShuttingDown_ = false;

        LOG_INFO("AppCore", "========================================");
        LOG_INFO("AppCore", "UnnamedBot - Desktop Pet Application");
        LOG_INFO("AppCore", "========================================");

        // 初始化日志系统（首先初始化）
        Logger::Instance().Initialize(L"./logs", 30);
        Logger::Instance().SetMinLevel(LogLevel::Debug);
        Logger::Instance().SetConsoleOutput(true);
        LOG_INFO("AppCore", "Logger initialized.");

        // 初始化数据库
        if (!Database::Instance().Initialize(L"./data/app.db")) {
            LOG_ERROR("AppCore", "Failed to initialize database.");
            return false;
        }
        LOG_INFO("AppCore", "Database initialized.");

        // 初始化配置管理器
        ConfigManager::Instance().Initialize();
        LOG_INFO("AppCore", "Config manager initialized.");

        // 初始化安全工具
        SecurityUtils::Instance().Initialize();
        LOG_INFO("AppCore", "Security utils initialized.");

        // 初始化附件管理器
        AttachmentManager::Instance().Initialize();
        LOG_INFO("AppCore", "Attachment manager initialized.");

        // 创建主窗口
        if (!CreateMainWindow(hInstance)) {
            LOG_ERROR("AppCore", "Failed to create main window.");
            return false;
        }
        LOG_INFO("AppCore", "Main window created.");

        // 初始化渲染器
        int width, height;
        mainWindow_->GetSize(width, height);
        bool vsync = ConfigManager::Instance().GetBool("renderer.vsync", false);
        if (!renderer_.Initialize(mainWindow_->GetHandle(), width, height, vsync)) {
            LOG_ERROR("AppCore", "Failed to initialize renderer.");
            return false;
        }
        LOG_INFO("AppCore", "Renderer initialized.");

        // 初始化Live2D
        if (!Live2DManager::Instance().Initialize()) {
            LOG_ERROR("AppCore", "Failed to initialize Live2D.");
            return false;
        }
        LOG_INFO("AppCore", "Live2D initialized.");

        // 初始化MCP工具
        MCPToolManager::Instance().RegisterDefaultTools();
        LOG_INFO("AppCore", "MCP tools registered.");

        // 初始化AI客户端
        if (!InitializeAIClient()) {
            LOG_WARN("AppCore", "AI client not configured. Some features may not work.");
        }

        // 初始化消息处理器
        MessageHandler::Instance().Initialize();
        LOG_INFO("AppCore", "Message handler initialized.");

        // 初始化任务调度器
        TaskScheduler::Instance().Initialize();
        LOG_INFO("AppCore", "Task scheduler initialized.");

        // 初始化更新管理器
        UpdateManager::Instance().Initialize();
        LOG_INFO("AppCore", "Update manager initialized.");

        // 初始化系统托盘
        if (!InitializeTrayIcon()) {
            LOG_WARN("AppCore", "Failed to initialize tray icon.");
        }

        // 注册全局热键
        RegisterHotkeys();

        // 加载默认模型
        LoadDefaultModel();

        // 设置消息处理器回调
        SetupCallbacks();

        // 启动更新线程
        updateThreadRunning_ = true;
        updateThread_ = std::thread(&AppCore::UpdateLoop, this);

        // 启动主窗口
        mainWindow_->Show(true);

        LOG_INFO("AppCore", "Application initialized successfully.");
        return true;
    }

    int AppCore::Run() {
        if (!isRunning_) {
            LOG_ERROR("AppCore", "Application not initialized.");
            return -1;
        }

        LOG_INFO("AppCore", "Application running.");

        // 显示启动通知
        TrayIcon::Instance().ShowNotification(
            L"UnnamedBot",
            L"Hello! I'm your desktop pet. Click me to chat!",
            NIIF_INFO,
            5000
        );

        // 进入主消息循环
        MainLoop();

        LOG_INFO("AppCore", "Application exiting.");
        return 0;
    }

    void AppCore::Shutdown() {
        if (isShuttingDown_) {
            return;
        }

        isShuttingDown_ = true;
        LOG_INFO("AppCore", "Shutting down application...");

        // 停止更新线程
        updateThreadRunning_ = false;
        if (updateThread_.joinable()) {
            updateThread_.join();
        }

        // 保存所有状态
        SaveState();
        Live2DManager::Instance().SaveModelState();
        Window::Instance().SaveState();

        // 注销热键
        UnregisterHotkeys();

        // 关闭各个模块
        TaskScheduler::Instance().Shutdown();
        UpdateManager::Instance().Shutdown();
        AttachmentManager::Instance().Shutdown();
        Live2DManager::Instance().Shutdown();
        renderer_.Shutdown();
        TrayIcon::Instance().Shutdown();
        Database::Instance().Close();
        Logger::Instance().~Logger();

        isRunning_ = false;
        LOG_INFO("AppCore", "Application shut down.");
    }

    void AppCore::SafeShutdown() {
        LOG_INFO("AppCore", "Safe shutdown initiated.");

        // 保存所有状态
        SaveState();
        Live2DManager::Instance().SaveModelState();
        Window::Instance().SaveState();

        // 发送退出消息
        PostQuitMessage(0);
    }

    void AppCore::ReloadConfig() {
        LOG_INFO("AppCore", "Reloading configuration...");
        ConfigManager::Instance().Initialize();
        // 重新应用配置
        bool topmost = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP, true);
        Window::Instance().SetAlwaysOnTop(topmost);
        bool clickThrough = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_CLICK_THROUGH, true);
        Window::Instance().SetClickThrough(clickThrough);

        // 更新AI配置
        InitializeAIClient();
    }

    void AppCore::SetTrayIconState(bool visible) {
        if (visible) {
            TrayIcon::Instance().Show(true);
        }
        else {
            TrayIcon::Instance().Hide();
        }
    }

    void AppCore::HandleHotkey(WPARAM wParam) {
        switch (wParam) {
        case HOTKEY_TOGGLE_MESSAGEBOX: {
            // 切换消息框
            if (TrayIcon::Instance().IsPetVisible()) {
                // 如果桌宠可见，切换消息框
                LOG_DEBUG("AppCore", "Hotkey: Toggle message box.");
            }
            break;
        }
        case HOTKEY_TOGGLE_TOPMOST: {
            // 切换置顶模式
            bool current = Window::Instance().IsAlwaysOnTop();
            Window::Instance().SetAlwaysOnTop(!current);
            ConfigManager::Instance().SetBool(ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP, !current);
            LOG_DEBUG("AppCore", "Hotkey: Toggle topmost to " + std::string(!current ? "true" : "false"));
            break;
        }
        case HOTKEY_DEBUG: {
            // 调试热键
            LOG_DEBUG("AppCore", "Debug hotkey pressed.");
            break;
        }
        }
    }

    HWND AppCore::GetMainWindow() const {
        return mainWindow_ ? mainWindow_->GetHandle() : nullptr;
    }

    // ============ 私有方法 ============

    bool AppCore::CreateMainWindow(HINSTANCE hInstance) {
        // 创建主窗口实例
        mainWindow_ = std::make_unique<Window>();

        // 从配置恢复窗口状态
        int x = ConfigManager::Instance().GetInt("window.position_x", 100);
        int y = ConfigManager::Instance().GetInt("window.position_y", 100);
        int width = ConfigManager::Instance().GetInt("window.width", 400);
        int height = ConfigManager::Instance().GetInt("window.height", 400);

        if (!mainWindow_->Create(L"UnnamedBot", width, height, x, y)) {
            LOG_ERROR("AppCore", "Failed to create main window.");
            return false;
        }

        // 设置窗口回调
        mainWindow_->OnMouseClick([this](int x, int y, bool isLeftDown) {
            if (isLeftDown) {
                // 左键单击：切换消息框
                LOG_DEBUG("AppCore", "Left click at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
                // 检查是否点击在模型区域
                if (Live2DManager::Instance().HitTest(x, y)) {
                    // 切换消息框
                    // TODO: 显示/隐藏消息框
                }
            }
            else {
                // 右键单击：显示上下文菜单
                LOG_DEBUG("AppCore", "Right click at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
                // TODO: 显示右键菜单（快速操作）
            }
            });

        mainWindow_->OnMouseDrag([this](int x, int y) {
            // 更新模型位置
            Live2DManager::Instance().SetModelPosition(static_cast<float>(x), static_cast<float>(y));
            });

        mainWindow_->OnWindowClose([this]() {
            // 窗口关闭时，隐藏窗口而不是退出
            mainWindow_->Hide();
            TrayIcon::Instance().ShowPet(false);
            LOG_DEBUG("AppCore", "Window closed.");
            });

        mainWindow_->OnWindowMove([this](int x, int y) {
            // 窗口移动时更新模型位置
            Live2DManager::Instance().SetModelPosition(static_cast<float>(x), static_cast<float>(y));
            });

        mainWindow_->OnWindowResize([this](int width, int height) {
            // 窗口大小变化时调整渲染器
            renderer_.Resize(width, height);
            });

        return true;
    }

    bool AppCore::InitializeTrayIcon() {
        HICON icon = LoadIcon(instance_, IDI_APPLICATION);
        if (!TrayIcon::Instance().Initialize(mainWindow_->GetHandle(), WM_TRAYICON, icon)) {
            return false;
        }

        // 设置托盘回调
        TrayIcon::Instance().OnShowPet([this]() {
            mainWindow_->Show(true);
            mainWindow_->RestoreState();
            LOG_DEBUG("AppCore", "Pet shown from tray.");
            });

        TrayIcon::Instance().OnHidePet([this]() {
            mainWindow_->Hide();
            LOG_DEBUG("AppCore", "Pet hidden from tray.");
            });

        TrayIcon::Instance().OnOpenMessageBox([this]() {
            // TODO: 显示消息框
            LOG_DEBUG("AppCore", "Open message box from tray.");
            });

        TrayIcon::Instance().OnOpenConfig([this]() {
            // TODO: 显示配置窗口
            LOG_DEBUG("AppCore", "Open config from tray.");
            });

        TrayIcon::Instance().OnSwitchModel([this]() {
            // TODO: 显示模型切换菜单
            LOG_DEBUG("AppCore", "Switch model from tray.");
            });

        TrayIcon::Instance().OnExit([this]() {
            SafeShutdown();
            });

        // 设置提示文本
        TrayIcon::Instance().SetTooltip(L"UnnamedBot - Desktop Pet");
        TrayIcon::Instance().Show(true);

        return true;
    }

    bool AppCore::InitializeLive2D() {
        return Live2DManager::Instance().Initialize();
    }

    bool AppCore::InitializeAIClient() {
        AIClientConfig config;
        config.baseUrl = ConfigManager::Instance().GetString(ConfigManager::KEY_API_BASE_URL, "https://api.openai.com/v1");
        config.apiKey = SecurityUtils::Instance().Decrypt(
            ConfigManager::Instance().GetString(ConfigManager::KEY_API_KEY, "")
        );
        config.organization = ConfigManager::Instance().GetString(ConfigManager::KEY_API_ORG_ID, "");
        config.model = ConfigManager::Instance().GetString(ConfigManager::KEY_API_MODEL_DEFAULT, "gpt-4o");
        config.temperature = ConfigManager::Instance().GetDouble(ConfigManager::KEY_SAMPLING_TEMPERATURE, 1.0);
        config.topP = ConfigManager::Instance().GetDouble(ConfigManager::KEY_SAMPLING_TOP_P, 1.0);
        config.frequencyPenalty = ConfigManager::Instance().GetDouble(ConfigManager::KEY_SAMPLING_FREQUENCY_PENALTY, 0.0);
        config.presencePenalty = ConfigManager::Instance().GetDouble(ConfigManager::KEY_SAMPLING_PRESENCE_PENALTY, 0.0);
        config.stream = ConfigManager::Instance().GetBool(ConfigManager::KEY_STREAM_ENABLED, true);
        config.timeoutSeconds = ConfigManager::Instance().GetInt(ConfigManager::KEY_STREAM_TIMEOUT_SECONDS, 30);
        config.proxyType = ConfigManager::Instance().GetString(ConfigManager::KEY_PROXY_TYPE, "none");
        config.proxyHost = ConfigManager::Instance().GetString(ConfigManager::KEY_PROXY_HOST, "");
        config.proxyPort = ConfigManager::Instance().GetInt(ConfigManager::KEY_PROXY_PORT, 0);

        if (config.apiKey.empty()) {
            LOG_WARN("AppCore", "API Key not configured. AI features will not work.");
            return false;
        }

        aiClient_.SetConfig(config);
        LOG_INFO("AppCore", "AI client initialized with model: " + config.model);
        return true;
    }

    bool AppCore::LoadDefaultModel() {
        std::string modelId = ConfigManager::Instance().GetString(ConfigManager::KEY_MODEL_DEFAULT_ID, "Haru");
        std::wstring modelPath = L"./models/" + std::wstring(modelId.begin(), modelId.end()) + L"/" +
            std::wstring(modelId.begin(), modelId.end()) + L".model3.json";

        if (!std::filesystem::exists(modelPath)) {
            // 尝试扫描模型目录
            Live2DManager::Instance().Initialize();
            // 如果没有找到模型，使用Haru示例
            std::wstring haruPath = L"./models/Haru/Haru.model3.json";
            if (std::filesystem::exists(haruPath)) {
                return Live2DManager::Instance().LoadModel("Haru", haruPath);
            }
            LOG_ERROR("AppCore", "No model found in ./models/ directory.");
            return false;
        }

        return Live2DManager::Instance().LoadModel(modelId, modelPath);
    }

    void AppCore::SetupCallbacks() {
        // 设置消息处理器回调
        MessageHandler::Instance().OnNewMessage([](const ConversationMessage& msg) {
            LOG_DEBUG("AppCore", "New message: " + msg.content.substr(0, 50) + "...");
            // 通知托盘有新消息
            TrayIcon::Instance().ShowNotification(
                L"New Message",
                std::wstring(msg.content.begin(), msg.content.end()).c_str(),
                NIIF_INFO,
                3000
            );
            });

        // 设置AI请求回调
        MessageHandler::Instance().SetAIRequestCallback([this](
            const std::vector<ConversationMessage>& history,
            std::vector<ConversationMessage>& aiMessages
            ) -> std::string {
                // 构建AI消息
                auto session = MessageHandler::Instance().GetCurrentSession();
                auto aiMsgList = MessageHandler::Instance().BuildAIMessages(
                    history,
                    session->systemPrompt,
                    session->prePrompt,
                    session->postPrompt
                );

                // 获取工具列表
                auto tools = MCPToolManager::Instance().GetToolDefinitions();

                // 发送请求（流式）
                std::string fullResponse;
                std::vector<ToolCall> pendingToolCalls;

                aiClient_.SendChatMessageStream(
                    aiMsgList,
                    [&fullResponse, &aiMessages](const std::string& chunk, bool complete) {
                        if (!chunk.empty()) {
                            fullResponse += chunk;
                        }
                        if (complete && !fullResponse.empty()) {
                            // 保存完整响应
                            ConversationMessage aiMsg;
                            aiMsg.role = MessageRole::Assistant;
                            aiMsg.content = fullResponse;
                            aiMessages.push_back(aiMsg);
                        }
                    },
                    [this, &pendingToolCalls](const ToolCall& toolCall) {
                        pendingToolCalls.push_back(toolCall);
                        LOG_DEBUG("AppCore", "Tool call: " + toolCall.function.name +
                            " with args: " + toolCall.function.arguments);
                    },
                    tools
                );

                // 处理工具调用
                if (!pendingToolCalls.empty()) {
                    // 执行工具并收集结果
                    std::vector<ToolResult> toolResults;
                    for (const auto& toolCall : pendingToolCalls) {
                        try {
                            nlohmann::json args = nlohmann::json::parse(toolCall.function.arguments);
                            auto result = MCPToolManager::Instance().ExecuteTool(toolCall.function.name, args);

                            ToolResult toolResult;
                            toolResult.toolCallId = toolCall.id;
                            toolResult.result = result.success ? result.content : result.error;
                            toolResult.success = result.success;
                            if (!result.success) {
                                toolResult.error = result.error;
                            }
                            toolResults.push_back(toolResult);

                            // 记录工具执行结果
                            ConversationMessage toolMsg;
                            toolMsg.role = MessageRole::Tool;
                            toolMsg.content = result.content;
                            toolMsg.extraData["tool_call_id"] = toolCall.id;
                            toolMsg.extraData["name"] = toolCall.function.name;
                            aiMessages.push_back(toolMsg);

                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("AppCore", "Tool execution failed: " + std::string(e.what()));
                            ToolResult toolResult;
                            toolResult.toolCallId = toolCall.id;
                            toolResult.result = "Error: " + std::string(e.what());
                            toolResult.success = false;
                            toolResult.error = std::string(e.what());
                            toolResults.push_back(toolResult);
                        }
                    }

                    // 提交工具结果（第二轮请求）
                    if (!toolResults.empty()) {
                        auto finalResponse = aiClient_.SubmitToolResults(aiMsgList, toolResults, tools);
                        if (!finalResponse.content.empty()) {
                            ConversationMessage finalMsg;
                            finalMsg.role = MessageRole::Assistant;
                            finalMsg.content = finalResponse.content;
                            aiMessages.push_back(finalMsg);
                            return finalResponse.content;
                        }
                    }
                }

                return fullResponse;
            });

        // 设置MCP确认回调
        MCPToolManager::Instance().SetConfirmationCallback([](const std::string& message) -> bool {
            int result = MessageBoxA(nullptr, message.c_str(), "Confirm Action", MB_YESNO | MB_ICONQUESTION);
            return result == IDYES;
            });
    }

    void AppCore::MainLoop() {
        MSG msg = {};
        while (isRunning_ && GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void AppCore::UpdateLoop() {
        LOG_DEBUG("AppCore", "Update thread started.");

        // 设置定时器间隔
        const int UPDATE_INTERVAL_MS = 100; // 100ms
        const int SAVE_INTERVAL_MS = SAVE_INTERVAL_SECONDS * 1000;
        const int TRIGGER_CHECK_INTERVAL_MS = 5000; // 5秒

        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastTriggerCheck = std::chrono::steady_clock::now();
        auto lastFrameTime = std::chrono::steady_clock::now();

        while (updateThreadRunning_ && !isShuttingDown_) {
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            // 更新Live2D模型
            Live2DManager::Instance().Update(deltaTime);

            // 更新任务调度器
            TaskScheduler::Instance().Update();

            // 检查更新
            UpdateManager::Instance().CheckUpdate();

            // 定时保存状态
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSaveTime).count() >= SAVE_INTERVAL_MS) {
                SaveState();
                lastSaveTime = now;
            }

            // 定时检查主动触发
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTriggerCheck).count() >= TRIGGER_CHECK_INTERVAL_MS) {
                CheckActiveTriggers();
                lastTriggerCheck = now;
            }

            // 渲染
            renderer_.BeginFrame();
            Live2DManager::Instance().Render(&renderer_);
            renderer_.EndFrame();

            // 帧率控制
            std::this_thread::sleep_for(std::chrono::milliseconds(UPDATE_INTERVAL_MS));
        }

        LOG_DEBUG("AppCore", "Update thread stopped.");
    }

    void AppCore::CheckActiveTriggers() {
        // 检查冷却
        if (triggerCooling_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTriggerTime_).count();
            int cooldown = ConfigManager::Instance().GetInt(ConfigManager::KEY_ACTIVE_COOLDOWN_SECONDS, 480);
            if (elapsed < cooldown) {
                return;
            }
            triggerCooling_ = false;
        }

        // 检查各个触发条件
        CheckProcessChange();
        CheckWeatherUpdate();
        CheckIPChange();
        CheckScreenCapture();
    }

    void AppCore::CheckProcessChange() {
        int interval = ConfigManager::Instance().GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_PROCESS, -1);
        if (interval < 0) return;

        // 简化：每N秒检查一次进程变化
        // 实际应由定时器驱动
        static int counter = 0;
        counter++;
        if (counter % 10 != 0) return; // 每10次检查一次（约50秒）

        auto processes = MCPUtils::GetFrontendProcesses();
        std::string currentList;
        for (const auto& proc : processes) {
            currentList += proc.at("name") + "|";
        }

        if (lastProcessList_ != currentList && !lastProcessList_.empty()) {
            // 进程列表发生变化
            LOG_INFO("AppCore", "Process list changed.");

            TriggerEvent event;
            event.type = TriggerEventType::ProcessChange;
            event.description = "Frontend applications changed";
            event.data["old"] = lastProcessList_;
            event.data["new"] = currentList;
            event.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

            MessageHandler::Instance().HandleTriggerEvent(event);
            triggerCooling_ = true;
            lastTriggerTime_ = std::chrono::steady_clock::now();
        }

        lastProcessList_ = currentList;
    }

    void AppCore::CheckWeatherUpdate() {
        int interval = ConfigManager::Instance().GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_WEATHER, -1);
        if (interval < 0) return;

        // 简化：每N秒检查一次天气
        static int counter = 0;
        counter++;
        if (counter % 60 != 0) return; // 每60次检查一次（约5分钟）

        std::string city = MCPUtils::GetCityByIP(MCPUtils::GetPublicIP());
        if (city.empty()) return;

        nlohmann::json params;
        params["city"] = city;
        auto result = MCPToolManager::Instance().ExecuteTool("get_weather", params);

        if (result.success && !result.data.empty()) {
            LOG_INFO("AppCore", "Weather updated for " + city);

            TriggerEvent event;
            event.type = TriggerEventType::WeatherUpdate;
            event.description = "Weather updated: " + result.content;
            event.data = result.data;
            event.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

            MessageHandler::Instance().HandleTriggerEvent(event);
            triggerCooling_ = true;
            lastTriggerTime_ = std::chrono::steady_clock::now();
        }
    }

    void AppCore::CheckIPChange() {
        bool checkOnStart = ConfigManager::Instance().GetBool(ConfigManager::KEY_ACTIVE_IP_CHECK_ON_START, true);
        if (!checkOnStart) return;

        // 获取当前IP和位置
        std::string currentIP = MCPUtils::GetPublicIP();
        if (currentIP.empty() || currentIP == lastIP_) return;

        std::string currentCity = MCPUtils::GetCityByIP(currentIP);
        if (currentCity.empty()) return;

        if (!lastCity_.empty() && lastCity_ != currentCity) {
            // 城市发生变化
            LOG_INFO("AppCore", "IP location changed: " + lastCity_ + " -> " + currentCity);

            TriggerEvent event;
            event.type = TriggerEventType::IPChange;
            event.description = "IP location changed from " + lastCity_ + " to " + currentCity;
            event.data["old_city"] = lastCity_;
            event.data["new_city"] = currentCity;
            event.data["old_ip"] = lastIP_;
            event.data["new_ip"] = currentIP;
            event.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

            MessageHandler::Instance().HandleTriggerEvent(event);
            triggerCooling_ = true;
            lastTriggerTime_ = std::chrono::steady_clock::now();
        }

        lastIP_ = currentIP;
        lastCity_ = currentCity;
    }

    void AppCore::CheckScreenCapture() {
        int interval = ConfigManager::Instance().GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_SCREENSHOT, -1);
        if (interval < 0) return;

        // 简化：每N秒检查一次
        static int counter = 0;
        counter++;
        if (counter % 180 != 0) return; // 每180次检查一次（约15分钟）

        LOG_DEBUG("AppCore", "Capturing screen for trigger...");

        // 获取屏幕截图
        std::vector<unsigned char> imageData;
        int width, height;
        if (!MCPUtils::CaptureScreen(imageData, width, height)) {
            LOG_ERROR("AppCore", "Failed to capture screen.");
            return;
        }

        TriggerEvent event;
        event.type = TriggerEventType::ScreenCapture;
        event.description = "Screen captured for analysis";
        event.data["width"] = width;
        event.data["height"] = height;
        event.data["size"] = imageData.size();
        event.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

        MessageHandler::Instance().HandleTriggerEvent(event);
        triggerCooling_ = true;
        lastTriggerTime_ = std::chrono::steady_clock::now();
    }

    void AppCore::SaveState() {
        // 保存窗口状态
        Window::Instance().SaveState();

        // 保存模型状态
        Live2DManager::Instance().SaveModelState();

        // 保存渲染器状态
        ConfigManager::Instance().SetBool("renderer.vsync", renderer_.GetVSync());
    }

    void AppCore::RegisterHotkeys() {
        // 获取热键配置
        std::string toggleMsg = ConfigManager::Instance().GetString(ConfigManager::KEY_HOTKEY_TOGGLE_MESSAGEBOX, "Ctrl+Alt+M");
        std::string toggleTop = ConfigManager::Instance().GetString(ConfigManager::KEY_HOTKEY_TOGGLE_TOPMOST, "Ctrl+Alt+T");

        // 注册热键
        // 简化：使用固定的热键
        if (RegisterHotKey(mainWindow_->GetHandle(), HOTKEY_TOGGLE_MESSAGEBOX, MOD_CONTROL | MOD_ALT, 'M')) {
            LOG_DEBUG("AppCore", "Hotkey registered: Ctrl+Alt+M");
        }
        if (RegisterHotKey(mainWindow_->GetHandle(), HOTKEY_TOGGLE_TOPMOST, MOD_CONTROL | MOD_ALT, 'T')) {
            LOG_DEBUG("AppCore", "Hotkey registered: Ctrl+Alt+T");
        }
        if (RegisterHotKey(mainWindow_->GetHandle(), HOTKEY_DEBUG, MOD_CONTROL | MOD_SHIFT, 'D')) {
            LOG_DEBUG("AppCore", "Hotkey registered: Ctrl+Shift+D");
        }
    }

    void AppCore::UnregisterHotkeys() {
        UnregisterHotKey(mainWindow_->GetHandle(), HOTKEY_TOGGLE_MESSAGEBOX);
        UnregisterHotKey(mainWindow_->GetHandle(), HOTKEY_TOGGLE_TOPMOST);
        UnregisterHotKey(mainWindow_->GetHandle(), HOTKEY_DEBUG);
        LOG_DEBUG("AppCore", "Hotkeys unregistered.");
    }

    // ============ 窗口过程 ============

    LRESULT CALLBACK AppCore::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        AppCore* pApp = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pApp = reinterpret_cast<AppCore*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pApp));
        }
        else {
            pApp = reinterpret_cast<AppCore*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pApp) {
            return pApp->HandleMessage(hwnd, msg, wParam, lParam);
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    LRESULT AppCore::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_TRAYICON: {
            TrayIcon::Instance().HandleTrayMessage(wParam, lParam);
            return 0;
        }

        case WM_HOTKEY: {
            HandleHotkey(wParam);
            return 0;
        }

        case WM_QUERYENDSESSION: {
            // 系统关机前保存状态
            SafeShutdown();
            return TRUE;
        }

        case WM_ENDSESSION: {
            // 系统关机
            if (wParam == TRUE) {
                SafeShutdown();
            }
            return 0;
        }

        case WM_CLOSE: {
            // 关闭窗口：隐藏而不是退出
            ShowWindow(hwnd, SW_HIDE);
            TrayIcon::Instance().ShowPet(false);
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

} // namespace UnnamedBot