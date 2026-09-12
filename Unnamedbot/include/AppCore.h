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

        // 应用状态
        std::atomic<bool> isRunning_ = false;
        std::atomic<bool> isShuttingDown_ = false;

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

        // 状态保存间隔（秒）
        static const int SAVE_INTERVAL_SECONDS = 5;

        // 窗口类名
        static const wchar_t* WINDOW_CLASS_NAME;

        // 托盘消息ID（WM_HOTKEY 是系统宏，不能作为成员名，热键消息直接使用系统 WM_HOTKEY）
        static constexpr UINT WM_TRAYICON = WM_USER + 100;

        // 热键ID
        static const int HOTKEY_TOGGLE_MESSAGEBOX = 1;
        static const int HOTKEY_TOGGLE_TOPMOST = 2;
        static const int HOTKEY_DEBUG = 3;

        // 冷却控制
        std::atomic<bool> triggerCooling_ = false;
        std::chrono::steady_clock::time_point lastTriggerTime_;
    };

} // namespace UnnamedBot