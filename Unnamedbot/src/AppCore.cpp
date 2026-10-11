#include "../include/AppCore.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Database.h"
#include "../include/Window.h"
#include "../include/ControlTerminalWindow.h"
#include "../include/SplashScreen.h"
#include "../include/Renderer.h"
#include "../include/Live2DManager.h"
#include "../include/ResourceIds.h"
#include "../include/AIClient.h"
#include "../include/MCPTool.h"
#include "../include/MessageHandler.h"
#include "../include/PromptLibrary.h"
#include "../include/Utils.h"
#include "../include/QuickInputBar.h"
#include "../include/SpeechBubble.h"
#include "../include/TrayIcon.h"
#include "../include/UiTheme.h"
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

    AppCore& AppCore::Instance() {
        // 不随静态存储期销毁：AppCore 析构发生在进程退出阶段，
        // 而窗口/渲染器的析构会回调托盘等单例，那些单例构造晚于 AppCore，
        // 静态析构时早已被销毁。资源释放由 Shutdown() 显式完成。
        static AppCore* instance = new AppCore();
        return *instance;
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
        // 日志与数据库都按「工作目录优先、否则 exe 目录」解析：
        // 装好之后从快捷方式启动时工作目录不可控，否则 data/logs 会散落到随机位置。
        Logger::Instance().Initialize(Utils::ResolveAppRelative(L"logs").wstring().c_str(), 30);
        // 发布版默认 info 级 + 不输出控制台：原来是 debug + 控制台，
        // 隐藏状态下 UpdateManager 每 100ms 打一行「Check interval not reached」，
        // 一分钟就是 860 行写盘 —— 老机器上这是实打实的磁盘与 CPU 开销。
        {
            const std::string level = Utils::ToLower(
                ConfigManager::Instance().GetString(ConfigManager::KEY_LOG_LEVEL, "info"));
            LogLevel parsed = LogLevel::Info;
            if (level == "debug") parsed = LogLevel::Debug;
            else if (level == "warn" || level == "warning") parsed = LogLevel::Warn;
            else if (level == "error" || level == "off" || level == "none") parsed = (level == "error" ? LogLevel::Error : LogLevel::Error);
            Logger::Instance().SetMinLevel(parsed);
            Logger::Instance().SetConsoleOutput(
                ConfigManager::Instance().GetBool(ConfigManager::KEY_LOG_CONSOLE, false));
        }
        LOG_INFO("AppCore", "Logger initialized.");

        // 贴图解码依赖 WIC（COM 组件），因此必须先初始化 COM。
        // 只有主线程会加载纹理，按单线程套间初始化即可。
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(comResult)) {
            comInitialized_ = true;
        }
        else if (comResult != RPC_E_CHANGED_MODE) {
            LOG_WARN("AppCore", "COM initialization failed. Error: " + std::to_string(comResult));
        }

        // 初始化数据库
        const std::filesystem::path dataDir = Utils::ResolveAppRelative(L"data");
        std::error_code dataEc;
        std::filesystem::create_directories(dataDir, dataEc);
        if (!Database::Instance().Initialize((dataDir / L"app.db").wstring().c_str())) {
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

        // 提示词目录：与 logs/data/models 同级。先建出来，用户才知道该把
        // .md/.txt 放哪里（「启用提示词」多选行读的就是这个目录）。
        PromptLibrary::Instance().EnsureFolder();
        // 快速模式的迷你输入条与屏幕气泡：都是按需弹的小窗口，这里只建类与句柄
        QuickInputBar::Instance().Initialize(hInstance);
        SpeechBubble::Instance().Initialize(hInstance);

        // 创建主窗口
        if (!CreateMainWindow(hInstance)) {
            LOG_ERROR("AppCore", "Failed to create main window.");
            return false;
        }
        LOG_INFO("AppCore", "Main window created.");

        // 从这里开始到配置界面出现之前都是同步初始化；最慢的一步（Live2D 模型
        // 加载需要编译约 500 个着色器程序）可达数十秒。先挂出加载界面，让等待可见。
        if (!SplashScreen::Instance().Create(hInstance)) {
            LOG_WARN("AppCore", "Failed to create splash screen; continuing without it.");
        }
        SplashScreen::Instance().SetStatus(L"正在初始化图形设备…", 0.10f);

        // 初始化渲染器
        int width, height;
        mainWindow_->GetSize(width, height);
        bool vsync = ConfigManager::Instance().GetBool("renderer.vsync", false);
        if (!renderer_.Initialize(mainWindow_->GetHandle(), width, height, vsync)) {
            LOG_ERROR("AppCore", "Failed to initialize renderer.");
            SplashScreen::Instance().Shutdown();
            return false;
        }
        LOG_INFO("AppCore", "Renderer initialized.");
        SplashScreen::Instance().SetStatus(L"正在初始化 Live2D 框架…", 0.22f);

        // 初始化Live2D框架；D3D11 渲染器需要在加载模型前拿到设备
        if (!Live2DManager::Instance().Initialize(&renderer_)) {
            LOG_ERROR("AppCore", "Failed to initialize Live2D.");
            SplashScreen::Instance().Shutdown();
            return false;
        }
        LOG_INFO("AppCore", "Live2D initialized.");
        SplashScreen::Instance().SetStatus(L"正在注册工具与 AI 服务…", 0.32f);

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
        SplashScreen::Instance().SetStatus(L"正在准备配置终端…", 0.45f);

        // 初始化控制终端（配置 GUI）
        if (!ControlTerminalWindow::Instance().Initialize(hInstance)) {
            LOG_WARN("AppCore", "Failed to initialize control terminal.");
        }
        // 保存即确认：启动流程故意先不显示桌宠，等用户在配置界面按下「保存」
        // 才把模型放到桌面上，否则未配置好的模型会直接跑起来。
        ControlTerminalWindow::Instance().OnSaved([this]() {
            ReloadConfig();
            if (mainWindow_ && !mainWindow_->IsVisible()) {
                mainWindow_->Show(true);
                mainWindow_->RestoreState();
                TrayIcon::Instance().ShowPet(true);
                LOG_INFO("AppCore", "Configuration saved; pet window shown.");
            }
        });

        // 初始化系统托盘
        if (!InitializeTrayIcon()) {
            LOG_WARN("AppCore", "Failed to initialize tray icon.");
        }

        // 注册全局热键
        RegisterHotkeys();

        // 加载默认模型：着色器编译全部集中在这一步，是整个启动中最耗时的阶段。
        // 模型加载挪到中枢启动之后（由更新线程执行）：
        // 载入 wuwuwu 这种 8192² 贴图要解码 PNG 再上传 256MB 纹理，
        // 放在这里会让用户对着加载画面干等；中枢先出来，模型随后就位即可。
        pendingModelLoad_ = true;
        SplashScreen::Instance().SetStatus(L"正在启动配置终端…", 0.80f);

        // 设置消息处理器回调
        SetupCallbacks();

        // 启动更新线程
        updateThreadRunning_ = true;
        updateThread_ = std::thread(&AppCore::UpdateLoop, this);

        // 启动流程：先打开配置界面，由用户决定何时显示桌宠。
        // 桌宠窗口在 Window::Create 中保持隐藏，可从托盘菜单「显示桌宠」唤起。
        renderer_.SetWindowOpacity(
            static_cast<float>(ConfigManager::Instance().GetDouble("window.opacity", 1.0)));

        // 配置界面即将出现，撤下加载界面（避免两个窗口重叠一瞬）
        SplashScreen::Instance().Shutdown();
        ControlTerminalWindow::Instance().Show(true);

        // 同步托盘状态：此时桌宠尚未显示，菜单项应呈现为「显示桌宠」
        TrayIcon::Instance().ShowPet(false);

        LOG_INFO("AppCore", "Application initialized successfully.");
        return true;
    }

    namespace {
        // 当前「有可见窗口的前端进程」清单（名字用 | 连起来，便于比较）。
        // 启动时先记一份当基线，第一次巡检就能发现变化，而不是先空转一个间隔。
        std::string CurrentFrontendProcessList() {
            std::string list;
            for (const auto& proc : MCPUtils::GetFrontendProcesses()) {
                auto it = proc.find("name");
                if (it != proc.end()) {
                    list += it->second + "|";
                }
            }
            return list;
        }
    } // namespace

    int AppCore::Run() {
        if (!isRunning_) {
            LOG_ERROR("AppCore", "Application not initialized.");
            return -1;
        }

        LOG_INFO("AppCore", "Application running.");

        // 主动触发：先把进程清单记一份基线。否则第一次巡检只能用来「建立基线」，
        // 用户看到的第一个间隔是空转的（表现就是「不按我设的时间来」）。
        lastProcessList_ = CurrentFrontendProcessList();
        LOG_INFO("AppCore", "Proactive trigger baselines primed (" +
            std::to_string(lastProcessList_.size()) + " bytes of process list).");

        // 把生效的排期打出来，便于核对「是否严格按自定义时间」
        {
            auto& cfg = ConfigManager::Instance();
            LOG_INFO("AppCore", "Proactive schedule: process=" +
                std::to_string(cfg.GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_PROCESS, 60)) + "s weather=" +
                std::to_string(cfg.GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_WEATHER, 300)) + "s screenshot=" +
                std::to_string(cfg.GetInt(ConfigManager::KEY_ACTIVE_INTERVAL_SCREENSHOT, 1800)) + "s cooldown=" +
                std::to_string(cfg.GetInt(ConfigManager::KEY_ACTIVE_COOLDOWN_SECONDS, 480)) + "s (<=0 = off)");
        }

        // 启动时不弹出任何提示：屏幕上只保留主窗口本身

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

        // 正常路径下配置界面出现时加载界面已撤销；这里兜底处理（例如初始化中途失败）。
        SplashScreen::Instance().Shutdown();
        LOG_INFO("AppCore", "Shutting down application...");

        // 停止更新线程
        updateThreadRunning_ = false;
        if (updateThread_.joinable()) {
            updateThread_.join();
        }

        // 保存所有状态
        SaveState();
        Live2DManager::Instance().SaveModelState();
        mainWindow_->SaveState();

        // 注销热键
        UnregisterHotkeys();

        // 关闭各个模块
        // 逐步打点：退出阶段的崩溃（ucrtbase 的「请求了严重的程序退出」）只能靠
        // 「最后一条日志停在哪一步」定位。
        LOG_DEBUG("AppCore", "Shutdown step: TaskScheduler");
        TaskScheduler::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: UpdateManager");
        UpdateManager::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: AttachmentManager");
        AttachmentManager::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: SpeechBubble");
        SpeechBubble::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: QuickInputBar");
        QuickInputBar::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: ControlTerminalWindow");
        ControlTerminalWindow::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: Live2DManager");
        Live2DManager::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: Renderer");
        renderer_.Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: TrayIcon");
        TrayIcon::Instance().Shutdown();
        LOG_DEBUG("AppCore", "Shutdown step: Database");
        Database::Instance().Close();
        LOG_DEBUG("AppCore", "Shutdown step: CoUninitialize");

        // 与 Initialize 中的 CoInitializeEx 配对（套间模式冲突时未曾初始化，不能反初始化）
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }

        isRunning_ = false;
        LOG_INFO("AppCore", "Application shut down.");

        // 到这里该收的都收好了：线程已 join、数据库已关、状态已保存、窗口已销毁。
        // 但 WebView2 的 controller->Close() 是异步的（浏览器进程还在收尾），
        // 而 CRT 的退出路径会再走一遍静态析构 + DLL 分离，正好落在
        // 「WebView2 还没退完就被 detach」的窗口里 —— 表现为 ucrtbase 抛
        // FAST_FAIL_FATAL_APP_EXIT（「请求了严重的程序退出」，退出码 0xC0000409）。
        // 所以这里直接结束进程，跳过那段析构。OS 会把剩下的资源一并回收。
        LOG_INFO("AppCore", "Terminating process to avoid the CRT exit path.");
        ::ExitProcess(0);
    }

    void AppCore::SafeShutdown() {
        LOG_INFO("AppCore", "Safe shutdown initiated.");

        // 保存所有状态
        SaveState();
        Live2DManager::Instance().SaveModelState();
        mainWindow_->SaveState();

        // 发送退出消息
        PostQuitMessage(0);
    }

    void AppCore::ReloadConfig() {
        LOG_INFO("AppCore", "Reloading configuration...");
        ConfigManager::Instance().Initialize();
        // 重新应用配置
        bool topmost = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP, true);
        mainWindow_->SetAlwaysOnTop(topmost);
        bool clickThrough = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_CLICK_THROUGH, true);
        mainWindow_->SetClickThrough(clickThrough);

        // 快速模式开关可能刚被改过：上一帧的命中掩码已经不代表现在的画面，
        // 丢掉它，免得刚切换的那几帧还是按模型轮廓判定点击。
        renderer_.InvalidateHitMask();

        // 窗口透明度改由合成视觉施加（见 Renderer::SetWindowOpacity）
        renderer_.SetWindowOpacity(
            static_cast<float>(ConfigManager::Instance().GetDouble("window.opacity", 1.0)));

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
            bool current = mainWindow_->IsAlwaysOnTop();
            mainWindow_->SetAlwaysOnTop(!current);
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

        // 不再强制吸附到右上角：窗口位置应按配置（window.position_x/y）还原

        // 命中判定源：窗口的 WM_NCHITTEST 每次都会问这里。
        // 普通模式用渲染器读回的 alpha 掩码（与看到的像素完全一致）；
        // 快速模式用圆点的几何范围（此时画面上只有一个圆点）。
        mainWindow_->SetHitTestProvider([this](int x, int y) { return HitTestClient(x, y); });

        // 设置窗口回调
        // 鼠标约定（两种模式统一）：右键 = 输入，左键 = 拖动/不拦截。
        // 快速模式原来左右键各弹一个输入条（左=面板式、右=下划线式），
        // 等于同一件事有两个入口；现在只留右键，弹的是原来左键那条面板式输入条。
        mainWindow_->OnMouseClick([this](int x, int y, bool isLeftDown) {
            if (isLeftDown) {
                LOG_DEBUG("AppCore", "Left click at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
                return;
            }

            LOG_DEBUG("AppCore", "Right click at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
            // Ctrl+右键 保留原来那套上下文菜单（配置终端 / 隐藏 / 总在最前 / 退出）。
            // 托盘右键也有同一套，这里留着只是为了不改变老习惯。
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                ShowContextMenu();
                return;
            }

            if (IsQuickModeActive()) {
                // 快速模式（圆点）：面板式输入条，贴着圆点弹出
                QuickInputBar::Instance().Popup(GetMainWindow(), [this](const std::string& text) {
                    // 发起网络请求前先脱离 UI 线程：否则请求期间窗口会卡住，
                    // 输入条也会弹不回来
                    std::thread([this, text]() { ProcessAIMessage(text); }).detach();
                    });
            }
            else {
                // 常规模式（模型）：极简输入条，整条透明，只剩一条下划线（？？？时是血红）。
                QuickInputBar::Instance().PopupUnderline(GetMainWindow(), [this](const std::string& text) {
                    std::thread([this, text]() { ProcessAIMessage(text); }).detach();
                    });
            }
            });

        // 拖拽与移动窗口都不设置「模型位置」：模型就画在窗口里，窗口动模型自然跟着动。
        // 而 Live2DManager::SetModelPosition 的入参是 CubismModelMatrix 的画布坐标
        // （SetCenterPosition 的偏移，单位是画布像素），把窗口的屏幕坐标喂进去
        // 既是错的量纲，还会在拖动时每帧写一次配置库。
        // 需要模型在窗口内微调时，应当用画布坐标调用它（0,0 = 居中）。

        mainWindow_->OnWindowClose([this]() {
            // 窗口关闭时，隐藏窗口而不是退出
            mainWindow_->Hide();
            TrayIcon::Instance().ShowPet(false);
            LOG_DEBUG("AppCore", "Window closed.");
            });

        mainWindow_->OnWindowResize([this](int width, int height) {
            // 窗口大小变化时调整渲染器
            renderer_.Resize(width, height);
            });

        // 中枢页面「退出」：页面把请求 PostMessage 到桌宠窗口，这里在消息循环里安全退出。
        // 绝不能在 WebView2 的消息回调里直接拆 webview——回调会返回到已释放的 COM 对象上，
        // 表现就是 ucrtbase 抛「请求了严重的程序退出」。
        mainWindow_->OnQuitRequested([this]() {
            LOG_INFO("AppCore", "Quit confirmed from hub UI; shutting down.");
            ControlTerminalWindow::Instance().Hide();
            SafeShutdown();
            });

        // 托盘回调与全局热键都注册在桌宠窗口上（见 InitializeTrayIcon / RegisterHotkeys），
        // 但处理逻辑在 AppCore。Window 不认这两类消息，不接这一步它们会被 DefWindowProc 丢掉：
        // 表现就是「托盘右键没反应」「热键没反应」。
        mainWindow_->OnTrayCallback(WM_TRAYICON, [](WPARAM wParam, LPARAM lParam) {
            TrayIcon::Instance().HandleTrayMessage(wParam, lParam);
            });
        mainWindow_->OnHotkey([this](int id) {
            HandleHotkey(static_cast<WPARAM>(id));
            });

        return true;
    }

    bool AppCore::InitializeTrayIcon() {
        // 托盘图标取自 exe 自身的 IDI_APPICON 资源（见 Unnamedbot.rc）。
        // 这里不退回 IDI_APPLICATION：本工程没有定义 UNICODE，该宏会展开成
        // MAKEINTRESOURCEA（LPSTR），与本行显式调用的 ...W 接口类型不匹配。
        // 资源缺失时把空句柄交给 TrayIcon::Initialize，由它自己兜底。
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
        if (!icon) {
            LOG_WARN("AppCore", "Application icon resource missing; tray falls back to the system icon.");
        }
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
            ControlTerminalWindow::Instance().Toggle();
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
        return Live2DManager::Instance().Initialize(&renderer_);
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
        // 模型目录同样是「工作目录优先、否则 exe 目录」：装好之后 exe 旁边才是 models
        const std::filesystem::path modelRoot =
            Utils::ResolveAppRelative(L"models");
        std::wstring modelPath = (modelRoot / std::wstring(modelId.begin(), modelId.end()) /
            (std::wstring(modelId.begin(), modelId.end()) + L".model3.json")).wstring();

        if (!std::filesystem::exists(modelPath)) {
            // 尝试扫描模型目录
            Live2DManager::Instance().Initialize(&renderer_);
            // 如果没有找到模型，使用Haru示例
            std::wstring haruPath = (modelRoot / L"Haru" / L"Haru.model3.json").wstring();
            if (std::filesystem::exists(haruPath)) {
                if (!Live2DManager::Instance().LoadModel("Haru", haruPath)) {
                    return false;
                }
                Live2DManager::Instance().SwitchModel("Haru");
                return true;
            }
            LOG_ERROR("AppCore", "No model found in ./models/ directory.");
            return false;
        }

        // 载入之后必须把它切成「当前模型」：Live2DManager::LoadModel 只在手上
        // 还没有模型时才自动切换（扫描目录时可能已经切过一次），否则
        // currentModelId_ 会停在旧值上，SaveModelState 又把旧 id 写回配置——
        // 表现就是「换了模型，下次启动又变回去」。
        if (!Live2DManager::Instance().LoadModel(modelId, modelPath)) {
            return false;
        }
        Live2DManager::Instance().SwitchModel(modelId);
        return true;
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

        MessageHandler::Instance().OnTriggerReply([](const std::string& reply) {
            // 主动触发的回复要「看得见」：贴到鼠标指针旁的气泡 + 一条托盘通知。
            // 这条路径不经过 ProcessAIMessage，所以之前触发了也没有任何显示。
            if (reply.empty()) {
                return;
            }
            LOG_INFO("AppCore", "Proactive trigger reply: " + reply.substr(0, 60));
            SpeechBubble::Instance().ShowNearActiveWindow(Utils::Utf8ToWide(reply));
            TrayIcon::Instance().ShowNotification(L"桌宠", Utils::Utf8ToWide(reply), NIIF_INFO, 5000);
            ControlTerminalWindow::Instance().PushChatHistory();
            });

        // 设置AI请求回调
        MessageHandler::Instance().SetAIRequestCallback([this](
            const std::vector<ConversationMessage>& history,
            std::vector<ConversationMessage>& aiMessages
            ) -> std::string {
                // 构建AI消息
                auto session = MessageHandler::Instance().GetCurrentSession();
                // 系统提示由 AppCore 组装：会话提示 + 启用的提示词文件 +（？？？时）人格
                auto aiMsgList = MessageHandler::Instance().BuildAIMessages(
                    history,
                    ComposeSystemPrompt(),
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

                // 传输/服务端失败：错误原因不再混在内容里，这里必须显式判断并放弃本轮，
                // 否则失败文本会被当成 AI 回复落库，之后每轮请求又把它发给模型，
                // 界面上就是同一个错误反复出现。
                if (aiClient_.LastRequestFailed()) {
                    LOG_WARN("AppCore", "AI request failed; nothing written to the conversation: " +
                        aiClient_.LastRequestError());
                    return std::string();
                }

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

    // ============ 快速模式 / 「？？？」 ============

    bool AppCore::IsQuickModeActive() const {
        return ConfigManager::Instance().GetBool(ConfigManager::KEY_QUICK_ENABLED, false);
    }

    bool AppCore::IsYandereMode() const {
        // 判据抽到了 UiTheme（各 UI 组件也要用它决定配色/字体），这里只做转发，
        // 避免「人格判定」在两处各写一份、改一处漏一处。
        return IsYandereModeActive();
    }

    float AppCore::QuickDotRadius() const {
        // 目标直径 24 像素（半径 12，单位是渲染缓冲像素）。
        // 定这个数的依据：托盘/任务栏用的是 ico 的 32px 档，「半个图标」约 16px
        // 直径对鼠标偏小；24px 在桌面上仍然是个小圆点，又刚好达到常用的
        // 最小舒适点击目标尺寸。
        // 旧值取窗口短边的 12%：400 的缓冲宽 -> 半径 48、直径 96 像素，
        // 实测（1920x1080 @125%）屏幕上就是 96 物理像素，明显过大。
        constexpr float kQuickDotDiameterPixels = 24.0f;
        return kQuickDotDiameterPixels * 0.5f;
    }

    bool AppCore::HitTestClient(int x, int y) const {
        int width = 0;
        int height = 0;
        renderer_.GetSize(width, height);

        // 坐标系：入参是客户端物理像素（见 Window.h 里 HitTestCallback 的约定），
        // 渲染缓冲（命中掩码、圆点半径）同样落在物理像素上——不要在这里乘除
        // dpiScale_。DirectComposition 是把交换链按 1:1 物理像素贴在窗口左上角，
        // 而不是按 DIP 缩放铺满窗口。实测（1920x1080 @125%，窗口 500x500 物理，
        // 缓冲 400x400）：画在缓冲中心 (200,200) 的圆点出现在客户端 (200,200)，
        // 直径正好 24 物理像素。若按 DPI 换算成 (250,250)，命中区会偏移 50px，
        // 变成「看得见圆点却点不中」。
        if (IsQuickModeActive()) {
            // 容差 4px：圆点很小，严格按半径判定会变得很难点中
            const float radius = QuickDotRadius() + 4.0f;
            const float dx = static_cast<float>(x) - static_cast<float>(width) * 0.5f;
            const float dy = static_cast<float>(y) - static_cast<float>(height) * 0.5f;
            return dx * dx + dy * dy <= radius * radius;
        }
        // 普通模式：渲染器在 Present 前读回的 alpha 掩码，就是画面上不透明的像素
        return renderer_.HitTestOpaque(x, y);
    }

    void AppCore::ShowContextMenu() {
        const HWND hwnd = GetMainWindow();
        if (!hwnd) {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        const bool petVisible = mainWindow_ && mainWindow_->IsVisible();
        AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"配置终端(&C)");
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE_PET, petVisible ? L"隐藏桌宠(&H)" : L"显示桌宠(&S)");
        AppendMenuW(menu, MF_STRING | (mainWindow_ && mainWindow_->IsAlwaysOnTop() ? MF_CHECKED : 0),
            IDM_TOGGLE_TOPMOST, L"总在最前(&T)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出(&X)");

        POINT cursor = {};
        GetCursorPos(&cursor);

        // 弹出菜单前必须先把窗口提到前台并 PostMessage(WM_NULL)：
        // 否则菜单不会在点击别处时消失（Windows 的经典要求）。
        SetForegroundWindow(hwnd);
        const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, hwnd, nullptr);
        PostMessage(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (command) {
        case IDM_OPEN_CONFIG:
            ControlTerminalWindow::Instance().Show(true);
            break;

        case IDM_TOGGLE_PET: {
            const bool show = !petVisible;
            if (mainWindow_) {
                mainWindow_->Show(show);
                if (show) {
                    mainWindow_->RestoreState();
                }
            }
            TrayIcon::Instance().ShowPet(show);
            break;
        }

        case IDM_TOGGLE_TOPMOST: {
            if (mainWindow_) {
                const bool next = !mainWindow_->IsAlwaysOnTop();
                mainWindow_->SetAlwaysOnTop(next);
                // 与热键行为一致：写回配置，重启后仍是同一个层级
                ConfigManager::Instance().SetBool(ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP, next);
            }
            break;
        }

        case IDM_EXIT:
            SafeShutdown();
            break;

        default:
            break;
        }
    }

    std::string AppCore::ComposeSystemPrompt() const {
        std::string prompt;
        const auto session = MessageHandler::Instance().GetCurrentSession();
        if (session) {
            prompt = session->systemPrompt;
        }

        // 用户启用的提示词文件（prompts 目录，可多选）
        prompt += PromptLibrary::Instance().BuildSystemPromptAppend(
            ConfigManager::Instance().GetString(ConfigManager::KEY_PROMPT_ENABLED, ""));

        // 「？？？」：硬编码人格放在最后——越靠后越贴近当前指令，
        // 才能压住用户自己写的长篇提示词
        if (IsYandereMode()) {
            prompt += "\n\n" + PromptLibrary::YanderePersona();
        }
        return prompt;
    }

    void AppCore::ProcessAIMessage(const std::string& userInput) {
        // 可能由 QuickInputBar 的回调在工作线程上调用，这里不碰 UI 对象
        if (userInput.empty() || isShuttingDown_) {
            return;
        }

        // 会话是消息落库与历史拼接的前提：数据库里一个会话都没有时先建一个
        if (!MessageHandler::Instance().GetCurrentSession()) {
            const int64_t sessionId = MessageHandler::Instance().CreateSession("Default Session");
            if (sessionId > 0) {
                MessageHandler::Instance().SwitchSession(sessionId);
            }
        }

        MessageHandler::Instance().AddMessage(MessageRole::User, userInput);

        const auto history = MessageHandler::Instance().GetCurrentMessages();
        const std::vector<Message> payload =
            MessageHandler::Instance().BuildAIMessages(history, ComposeSystemPrompt(), "", "");

        std::string reply;
        aiClient_.SendChatMessageStream(payload, [&reply](const std::string& chunk, bool /*complete*/) {
            if (!chunk.empty()) {
                reply += chunk;
            }
            });

        // 用户主动发起的对话：失败要看得见，但绝不能落库成「AI 回复」——
        // 那会把 "Error: HTTP 400" 变成会话历史的一部分并被反复重发。
        if (aiClient_.LastRequestFailed()) {
            const std::string error = aiClient_.LastRequestError();
            LOG_WARN("AppCore", "AI request failed (not stored as a reply): " + error);
            const std::wstring text = L"请求失败：" + Utils::Utf8ToWide(error);
            TrayIcon::Instance().ShowNotification(L"AI 请求失败", text, NIIF_WARNING, 5000);
            if (IsQuickModeActive() && !isShuttingDown_) {
                SpeechBubble::Instance().ShowNearActiveWindow(text);
            }
            return;
        }

        reply = Utils::Trim(reply);
        if (reply.empty()) {
            LOG_WARN("AppCore", "AI reply is empty.");
            return;
        }

        MessageHandler::Instance().AddAIResponse(reply);

        // 快速模式：回复直接贴到屏幕上（没有 Live2D 可看，托盘气泡太容易错过）
        if (IsQuickModeActive() && !isShuttingDown_) {
            SpeechBubble::Instance().ShowNearActiveWindow(Utils::Utf8ToWide(reply));
        }
    }

    void AppCore::MainLoop() {
        MSG msg = {};
        while (isRunning_ && GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void AppCore::SubmitUserMessage(const std::string& text) {
        ProcessAIMessage(text);
        // 回复（或失败提示）已经落库：让中枢页面的对话屏刷新一次。
        // PushChatHistory 内部只 PostMessage，所以从工作线程调用是安全的。
        ControlTerminalWindow::Instance().PushChatHistory();
    }

    AIClient::ProviderProbe AppCore::ProbeProvider(const std::string& baseUrl, const std::string& apiKey) {
        // ProbeProvider 只读 config_（地址与密钥是显式传入的），不碰会话状态，
        // 所以探测可以和正在进行的对话并存，不需要额外加锁。
        return aiClient_.ProbeProvider(baseUrl, apiKey);
    }

    void AppCore::RefreshPet() {
        LOG_INFO("AppCore", "Refreshing pet (model + config + window).");

        // 1) 重读配置：位置、缩放、不透明度、点击穿透、快速模式都在里面
        ReloadConfig();

        // 2) 重新载入模型：换过模型 id 或模型文件被动过时，这一步才是真正生效的地方。
        //    Unload 与 Load 成对出现，免得旧模型的纹理一直挂在 GPU 上。
        const std::string modelId = ConfigManager::Instance().GetString(ConfigManager::KEY_MODEL_DEFAULT_ID, "Haru");
        Live2DManager::Instance().UnloadModel(modelId);
        if (!LoadDefaultModel()) {
            LOG_WARN("AppCore", "Refresh: model reload failed, keeping previous state.");
        }

        // 3) 把窗口摆回来：位置从配置读，并且确保它是可见的
        if (mainWindow_) {
            mainWindow_->SetPosition(
                ConfigManager::Instance().GetInt("window.position_x", 100),
                ConfigManager::Instance().GetInt("window.position_y", 100));
            if (!mainWindow_->IsVisible()) {
                mainWindow_->Show(true);
            }
            mainWindow_->RestoreState();
            TrayIcon::Instance().ShowPet(true);
        }

        // 4) 命中掩码是按上一帧画面算的，模型换过就必须丢掉
        renderer_.InvalidateHitMask();

        // 5) 给用户一个看得见的反馈：紧凑模式贴气泡，否则发一条托盘通知
        const std::wstring text = L"桌宠已刷新";
        if (IsQuickModeActive() && !isShuttingDown_) {
            SpeechBubble::Instance().ShowNearActiveWindow(text);
        }
        TrayIcon::Instance().ShowNotification(L"桌宠中枢", text, NIIF_INFO, 2500);
    }

    void AppCore::UpdateLoop() {
        LOG_DEBUG("AppCore", "Update thread started.");

        // 帧间隔、命中掩码节奏、落库间隔都做成配置项：
        // 老设备把帧间隔调大即可线性降低 CPU/GPU 占用，低配模式再降一档。
        const bool lowEnd = ConfigManager::Instance().GetBool(ConfigManager::KEY_PERF_LOW_END, false);
        int frameIntervalMs = ConfigManager::Instance().GetInt(
            ConfigManager::KEY_PERF_FRAME_INTERVAL_MS, lowEnd ? 150 : 100);
        if (lowEnd) {
            frameIntervalMs = (std::max)(frameIntervalMs, 150);
        }
        frameIntervalMs = (std::clamp)(frameIntervalMs, 33, 1000);   // 30FPS 上限、1FPS 下限
        const int hitMaskEvery = (std::max)(1, ConfigManager::Instance().GetInt(
            ConfigManager::KEY_PERF_HITMASK_EVERY, lowEnd ? 6 : 3));
        const int saveIntervalSeconds = (std::max)(5, ConfigManager::Instance().GetInt(
            ConfigManager::KEY_PERF_SAVE_INTERVAL, lowEnd ? 60 : 30));
        const int SAVE_INTERVAL_MS = saveIntervalSeconds * 1000;
        const int TRIGGER_CHECK_INTERVAL_MS = 1000; // 1 秒：主动触发的计时分辨率
        // 桌宠隐藏时不再逐帧推进模型/渲染，改为低频巡检
        const int kHiddenSleepMs = 500;
        int frameIndex = 0;
        LOG_INFO("AppCore", "Performance: frame=" + std::to_string(frameIntervalMs) + "ms, hitmaskEvery=" +
            std::to_string(hitMaskEvery) + ", saveEvery=" + std::to_string(saveIntervalSeconds) +
            "s, lowEnd=" + (lowEnd ? "on" : "off"));

        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastTriggerCheck = std::chrono::steady_clock::now();
        auto lastFrameTime = std::chrono::steady_clock::now();

        while (updateThreadRunning_ && !isShuttingDown_) {
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            // 挂起的模型加载：在渲染线程上做，避免和主线程争 D3D11 上下文
            if (pendingModelLoad_.exchange(false)) {
                const auto loadStart = std::chrono::steady_clock::now();
                LoadDefaultModel();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - loadStart).count();
                LOG_INFO("AppCore", "Deferred model load finished in " + std::to_string(ms) + " ms.");
            }

            const bool petVisible = (mainWindow_ && mainWindow_->IsVisible());

            // 桌宠隐藏时不推进模型与物理：这是隐藏态最省的一步。
            // deltaTime 仍然夹一下，避免重新显示时物理一步跳太远。
            if (petVisible) {
                Live2DManager::Instance().Update((std::min)(deltaTime, 0.25f));
            }

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

            // 渲染（桌宠隐藏时不必绘制，避免无谓占用 GPU）
            if (petVisible) {
                renderer_.BeginFrame();
                if (IsQuickModeActive()) {
                    // 快速模式：完全不进 Live2D，只在窗口中心画一个圆点。
                    // 窗口尺寸不改：不透明区域靠命中判定控制，改尺寸反而会
                    // 把 window.width/height 写进配置（SaveState 会存当前尺寸）。
                    int width = 0;
                    int height = 0;
                    renderer_.GetSize(width, height);
                    const float radius = QuickDotRadius();
                    // 圆点颜色跟主题走：？？？ 时是血色，普通模式是冰蓝。
                    // 原来这里写死了冰蓝，切到 ？？？ 后屏幕上唯一的可见物还是蓝色，
                    // 与整套皮肤对不上。
                    const UiTheme theme = CurrentUiTheme();
                    const DirectX::XMFLOAT4 color(
                        GetRValue(theme.accent) / 255.0f,
                        GetGValue(theme.accent) / 255.0f,
                        GetBValue(theme.accent) / 255.0f,
                        1.0f);
                    renderer_.DrawDot(width * 0.5f, height * 0.5f, radius, color);
                }
                else {
                    Live2DManager::Instance().Render(&renderer_);
                }
                // 命中掩码必须在画完、Present 之前读回：它决定下一轮
                // WM_NCHITTEST 的「人物哪里能点」。但这是一次 GPU→CPU 的同步读回，
                // 是每帧最贵的一步，而它只在模型/动作变化时才变 —— 按配置每 N 帧读一次。
                if (hitMaskEvery <= 1 || (frameIndex % hitMaskEvery) == 0) {
                    renderer_.CaptureHitMask();
                }
                renderer_.EndFrame();
            }

            ++frameIndex;

            // 帧率控制：可见时按配置的帧间隔，隐藏时低频巡检
            std::this_thread::sleep_for(std::chrono::milliseconds(petVisible ? frameIntervalMs : kHiddenSleepMs));
        }

        LOG_DEBUG("AppCore", "Update thread stopped.");
    }

    namespace {
        bool DueAt(std::chrono::steady_clock::time_point& next, int intervalSeconds, const char* name) {
            if (intervalSeconds <= 0) {
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (next.time_since_epoch().count() == 0) {
                next = now + std::chrono::seconds(intervalSeconds);
                LOG_DEBUG("AppCore", std::string("Trigger '") + name + "' scheduled every " +
                    std::to_string(intervalSeconds) + "s.");
                return false;
            }
            if (now < next) {
                return false;
            }
            // 从「上次计划时刻」推进，而不是从 now 起算：
            // 否则每次都会把检查本身的开销累加进去，跑久了越差越多。
            next += std::chrono::seconds(intervalSeconds);
            if (next < now) {
                // 落后超过一个整周期（休眠 / 卡顿）→ 重新对齐，避免补跑一串
                const auto behind = std::chrono::duration_cast<std::chrono::seconds>(now - next).count();
                if (behind >= intervalSeconds) {
                    next = now + std::chrono::seconds(intervalSeconds);
                }
            }
            LOG_DEBUG("AppCore", std::string("Trigger '") + name + "' fired on schedule.");
            return true;
        }
    } // namespace

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
        if (!DueAt(nextProcessCheck_, interval, "process")) return;

        const std::string currentList = CurrentFrontendProcessList();
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

        if (!DueAt(nextWeatherCheck_, interval, "weather")) return;

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

        // 配置键的名字就是「启动时检查 IP」：查一次就够。
        // 原来它被 CheckActiveTriggers 每 5 秒调一次，而这里每次都发一次公网 IP 请求——
        // 等于每 5 秒打一次网络，既是浪费，也会把触发冷却一直占着。
        if (ipCheckedOnce_) return;
        ipCheckedOnce_ = true;

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

        if (!DueAt(nextScreenshotCheck_, interval, "screenshot")) return;

        // 交给视觉模型真正「看一眼」。抓屏 + 编码 + 网络都在工作线程里做，
        // 这里只占住冷却并派出去，别让渲染循环等它。
        LOG_INFO("AppCore", "Screenshot trigger fired; analysing with the vision model.");
        triggerCooling_ = true;
        lastTriggerTime_ = std::chrono::steady_clock::now();
        AnalyzeScreenshot();
    }

    void AppCore::AnalyzeScreenshot() {
        // 上一张还在分析就跳过这一轮：宁可少看一次，也不要叠请求
        if (screenshotInFlight_.exchange(true)) {
            LOG_DEBUG("AppCore", "Screenshot analysis already running; skipping this round.");
            return;
        }

        std::thread([this]() {
            struct Guard {
                std::atomic<bool>& flag;
                ~Guard() { flag = false; }
            } guard{ screenshotInFlight_ };

            // 工作线程里绝不能让异常逃出去：detached 线程上未捕获的异常
            // 会直接 std::terminate 掉整个进程（表现是「一触发就崩」）。
            try {
            // 工作线程要用 COM（WIC 编码）：自己初始化一次
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            std::vector<unsigned char> pixels;
            int width = 0;
            int height = 0;
            if (!MCPUtils::CaptureScreen(pixels, width, height) || pixels.empty()) {
                LOG_ERROR("AppCore", "Screen capture failed; screenshot trigger skipped.");
                if (SUCCEEDED(com)) CoUninitialize();
                return;
            }

            // 裸像素既不是模型认的图片格式，塞进请求也会撑爆体积：先编成 JPEG
            const std::vector<uint8_t> jpeg = Utils::EncodeJpegBgra(
                pixels.data(), width, height, width * 4, 80);
            if (jpeg.empty()) {
                LOG_ERROR("AppCore", "JPEG encoding failed; screenshot trigger skipped.");
                if (SUCCEEDED(com)) CoUninitialize();
                return;
            }
            LOG_INFO("AppCore", "Screenshot encoded: " + std::to_string(width) + "x" +
                std::to_string(height) + " · " + std::to_string(jpeg.size() / 1024) + " KB JPEG");

            Message systemMsg;
            systemMsg.role = MessageRole::System;
            systemMsg.content = ComposeSystemPrompt();

            Message userMsg;
            userMsg.role = MessageRole::User;
            userMsg.content =
                "（系统提示：这是他自己屏幕上的画面，你刚偷看了一眼。"
                "不要罗列画面里有什么，用一句话说出你注意到的、和他有关的事。）";
            nlohmann::json attachment;
            attachment["type"] = "image";
            attachment["mime_type"] = "image/jpeg";
            attachment["url"] = "";          // 显式给出：走 base64 分支
            attachment["base64"] = Utils::Base64Encode(jpeg);
            userMsg.attachments.push_back(attachment);

            // 视觉模型单独一项配置：默认就是 DeepSeek 带视觉的 deepseek-flash
            const std::string visionModel = ConfigManager::Instance().GetString(
                ConfigManager::KEY_API_MODEL_VISION, "deepseek-flash");
            LOG_DEBUG("AppCore", "Vision analysis via model: " + visionModel);

            const auto response = aiClient_.SendChatMessage(
                { systemMsg, userMsg }, {}, "none", visionModel);
            LOG_INFO("AppCore", "Vision request returned: content=" +
                std::to_string(response.content.size()) + " bytes, failed=" +
                (aiClient_.LastRequestFailed() ? ("yes (" + aiClient_.LastRequestError() + ")") : "no"));

            if (response.content.empty() || aiClient_.LastRequestFailed()) {
                LOG_WARN("AppCore", "Vision analysis failed: " + aiClient_.LastRequestError());
                if (SUCCEEDED(com)) CoUninitialize();
                return;
            }

            // 落库，让对话页也能看到这次「偷看」
            auto& handler = MessageHandler::Instance();
            handler.AddMessage(MessageRole::User, "（看一眼屏幕）", nlohmann::json::object(),
                "trigger_screencapture", true);
            handler.AddAIResponse(response.content);

            LOG_INFO("AppCore", "Vision trigger reply: " + response.content.substr(0, 60));
            SpeechBubble::Instance().ShowNearActiveWindow(Utils::Utf8ToWide(response.content));
            TrayIcon::Instance().ShowNotification(L"桌宠", Utils::Utf8ToWide(response.content), NIIF_INFO, 5000);
            ControlTerminalWindow::Instance().PushChatHistory();

            if (SUCCEEDED(com)) CoUninitialize();
            }
            catch (const std::exception& e) {
                LOG_ERROR("AppCore", std::string("Screenshot analysis threw: ") + e.what());
            }
            catch (...) {
                LOG_ERROR("AppCore", "Screenshot analysis threw an unknown exception.");
            }
            }).detach();
    }

    void AppCore::SaveState() {
        // 保存窗口状态
        mainWindow_->SaveState();

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