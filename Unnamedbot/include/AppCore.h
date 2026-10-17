#pragma once

#include <Windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

#include "Window.h"
#include "Renderer.h"
#include "AIClient.h"
#include "MessageHandler.h"

namespace UnnamedBot {

    // 应用核心控制器（单例）
    class AppCore {
    public:
        static AppCore& Instance();

        // 初始化应用
        bool Initialize(HINSTANCE hInstance, int nCmdShow);

        // 运行应用（主循环）
        int Run();

        // 关闭应用
        void Shutdown();

        // 检查是否正在运行
        bool IsRunning() const { return isRunning_; }

        // 获取窗口句柄
        HWND GetMainWindow() const;

        // 获取配置
        void ReloadConfig();

        // 设置系统托盘状态
        void SetTrayIconState(bool visible);

        // 处理全局热键
        void HandleHotkey(WPARAM wParam);

        // 安全退出（保存所有状态）
        void SafeShutdown();

        // 中枢「对话」屏的入口：把一句话交给完整链路（落库 → 组装 → 流式请求），
        // 结束后把会话重新推给配置终端页面。可在任意线程调用（内部走 ProcessAIMessage）。
        void SubmitUserMessage(const std::string& text);

        // 中枢 API 屏的探测：用当前（可能还没保存的）地址与密钥问一次服务商的模型清单。
        // 复用 AIClient 的代理与超时设置；可在任意线程调用。
        AIClient::ProviderProbe ProbeProvider(const std::string& baseUrl, const std::string& apiKey);

        // 截屏巡检：抓屏 → JPEG → base64 → 交给配置里的视觉模型看一眼，
        // 回复落库并弹出来。含编码与网络，所以自己开工作线程跑。
        void AnalyzeScreenshot();

        // 刷新桌宠（中枢主菜单的「刷新」）：重读配置 → 重新载入模型 → 把窗口摆回来。
        // 与「保存」走同一条应用链路，只是不写库。
        void RefreshPet();

    private:
        AppCore() = default;
        ~AppCore() = default;
        AppCore(const AppCore&) = delete;
        AppCore& operator=(const AppCore&) = delete;

        // 初始化各个模块
        bool InitializeModules();

        // 设置各模块之间的回调
        void SetupCallbacks();

        // 创建主窗口
        bool CreateMainWindow(HINSTANCE hInstance);

        // 初始化系统托盘
        bool InitializeTrayIcon();

        // 初始化Live2D
        bool InitializeLive2D();

        // 初始化AI客户端
        bool InitializeAIClient();

        // 加载默认模型
        bool LoadDefaultModel();

        // 主循环（Windows消息循环）
        void MainLoop();

        // 快速模式：「关闭 Live2D，只在屏幕上留一个圆点」
        bool IsQuickModeActive() const;

        // 「？？？」人格（快速模式 + 哨兵文件存在时用户选了它）
        bool IsYandereMode() const;

        // 命中判定（客户端物理像素）。由窗口的 WM_NCHITTEST 逐次调用：
        // 普通模式查渲染器读回的 alpha 掩码，快速模式算到圆心的距离。
        bool HitTestClient(int x, int y) const;

        // 快速模式下圆点的半径（物理像素）。绘制与命中判定共用同一算式，
        // 否则「看到的圆」与「能点的区域」会不一致。
        float QuickDotRadius() const;

        // 桌宠右键菜单（配置终端 / 显示·隐藏 / 置顶 / 退出）
        void ShowContextMenu();

        // 组装系统提示：会话提示 + 启用的提示词文件 +（？？？时）硬编码人格
        std::string ComposeSystemPrompt() const;

        // 更新循环（定时器驱动）
        void UpdateLoop();

        // 主动触发检测
        void CheckActiveTriggers();

        // 检查进程变化
        void CheckProcessChange();

        // 检查天气更新
        void CheckWeatherUpdate();

        // 检查IP变化
        void CheckIPChange();

        // 检查屏幕截图（定时）
        void CheckScreenCapture();

        // 保存状态（定时）
        void SaveState();

        // 处理AI消息
        void ProcessAIMessage(const std::string& userInput);

        // 处理主动触发AI消息
        void ProcessTriggerMessage(const TriggerEvent& event);

        // 全局热键注册
        void RegisterHotkeys();

        // 注销全局热键
        void UnregisterHotkeys();

        // 窗口过程（静态）
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 窗口过程（非静态）
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // WebView2 配置界面不可用时的降级：直接显示桌宠窗口（隐藏空白配置窗口）。
        void FallbackShowPet();

        // 应用状态
        std::atomic<bool> isRunning_ = false;
        std::atomic<bool> isShuttingDown_ = false;

        // COM 是否由本类初始化（用于配对 CoUninitialize）
        bool comInitialized_ = false;

        // 窗口
        std::unique_ptr<Window> mainWindow_;
        HINSTANCE instance_ = nullptr;

        // 渲染器
        Renderer renderer_;

        // AI客户端
        AIClient aiClient_;

        // 更新线程
        std::thread updateThread_;
        std::atomic<bool> updateThreadRunning_ = false;

        // 定时器
        UINT_PTR saveTimer_ = 0;
        UINT_PTR processTimer_ = 0;
        UINT_PTR weatherTimer_ = 0;
        UINT_PTR screenshotTimer_ = 0;
        UINT_PTR ipCheckTimer_ = 0;

        // 上次检测状态（用于比较）
        std::string lastProcessList_;
        std::string lastIP_;
        std::string lastCity_;
        double lastLatitude_ = 0.0;
        double lastLongitude_ = 0.0;

        // 主动触发各自的「下次该检查的时刻」。
        // 原来这三项巡检写的是 static counter % N 的硬编码节拍，配置里的
        // active.interval_* 秒数完全不起作用；改成按配置间隔各自计时。
        // 截屏分析进行中：一次没跑完就跳过这一轮，不叠请求
        std::atomic<bool> screenshotInFlight_ = false;

        // 模型加载挂起标记：Initialize 里只置位，真正的加载放到更新线程的第一帧，
        // 这样配置中枢窗口不必等模型（8192² 贴图解码+上传很慢）。
        std::atomic<bool> pendingModelLoad_ = false;

        // WebView2 降级：配置界面不可用 → 需要把桌宠放出来（见 FallbackShowPet）。
        // 失败可能发生在 Initialize 尚未走完时（同步环境失败），所以先记标志，
        // Initialize 末尾统一补做，避免托盘/模型还没就绪就调 ShowPet。
        std::atomic<bool> fallbackPetPending_ = false;
        // 降级只做一次：环境创建失败、控制器失败、看门狗超时会依次上报，
        // 不能每报一次就把桌宠窗口重摆一遍。
        std::atomic<bool> fallbackPetShown_ = false;
        bool initializationDone_ = false;

        std::chrono::steady_clock::time_point nextProcessCheck_{};
        std::chrono::steady_clock::time_point nextWeatherCheck_{};
        std::chrono::steady_clock::time_point nextScreenshotCheck_{};
        bool ipCheckedOnce_ = false;   // active.ip_check_on_start：启动时查一次即可

        // 状态保存间隔（秒）
        // 状态落库间隔（秒）。原来是 5 秒，对老机器的磁盘是持续的小写入；
        // 现在 30 秒一次，并可由 performance.save_interval_seconds 覆盖。
        static const int SAVE_INTERVAL_SECONDS = 30;

        // 窗口类名
        static const wchar_t* WINDOW_CLASS_NAME;

        // 托盘消息ID（WM_HOTKEY 是系统宏，不能作为成员名，热键消息直接使用系统 WM_HOTKEY）
        static constexpr UINT WM_TRAYICON = WM_USER + 100;

        // 热键ID
        static const int HOTKEY_TOGGLE_MESSAGEBOX = 1;
        static const int HOTKEY_TOGGLE_TOPMOST = 2;
        static const int HOTKEY_DEBUG = 3;

        // 右键菜单命令ID（自定义范围，避开系统与托盘已用的值）
        static constexpr UINT IDM_OPEN_CONFIG = 40001;
        static constexpr UINT IDM_TOGGLE_PET = 40002;
        static constexpr UINT IDM_TOGGLE_TOPMOST = 40003;
        static constexpr UINT IDM_EXIT = 40004;

        // 冷却控制
        std::atomic<bool> triggerCooling_ = false;
        std::chrono::steady_clock::time_point lastTriggerTime_;
    };

} // namespace UnnamedBot