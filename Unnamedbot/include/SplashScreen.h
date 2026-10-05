#pragma once

#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace UnnamedBot {

    // 启动加载界面。
    //
    // 应用初始化里有一段很长的同步阻塞（首次加载 Live2D 模型需要编译约 500 个
    // 着色器程序，通常数十秒），期间主线程无法抽空消息队列，任何属于主线程的
    // 窗口都会停止重绘。因此加载界面运行在**自己的线程**上：窗口消息由该线程
    // 独立泵送，主线程只通过 SetStatus 投递状态（加锁写数据 + PostMessage 唤醒），
    // 因此进度、计时与流动高光在整个初始化期间都不会冻结。
    class SplashScreen {
    public:
        static SplashScreen& Instance();

        // 创建并显示加载界面（在工作线程上建窗）。创建失败返回 false，
        // 调用方应继续启动流程——加载界面不是必需的。
        bool Create(HINSTANCE hInstance);

        // 关闭并销毁加载界面，等待工作线程退出。
        void Shutdown();

        bool IsRunning() const { return hwnd_.load() != nullptr; }

        // 更新状态文本与进度（0.0 – 1.0）。非阻塞，可在初始化线程的任意位置调用。
        void SetStatus(const std::wstring& text, float progress);

        // 只更新文本，保持当前进度。
        void SetStatus(const std::wstring& text);

    private:
        SplashScreen() = default;
        ~SplashScreen() = default;
        SplashScreen(const SplashScreen&) = delete;
        SplashScreen& operator=(const SplashScreen&) = delete;

        // 工作线程入口：注册窗口类 → 建窗 → 跑自己的消息循环
        void ThreadMain(HINSTANCE hInstance);

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        LRESULT OnCreate(HWND hwnd);
        void OnPaint(HWND hwnd);
        void OnDestroy();

        void CreateFonts();
        void DestroyFonts();

        // 逻辑像素（96 DPI）转物理像素
        int Scale(int value) const;

        // 状态（初始化线程写，界面线程读）
        mutable std::mutex statusMutex_;
        std::wstring status_ = L"正在启动…";
        float progress_ = 0.0f;

        std::atomic<HWND> hwnd_{ nullptr };
        std::thread thread_;

        // Create() 与工作线程之间的建窗握手
        std::mutex readyMutex_;
        std::condition_variable readyCv_;
        bool ready_ = false;
        bool created_ = false;

        HINSTANCE instance_ = nullptr;
        int dpi_ = 96;

        HFONT titleFont_ = nullptr;
        HFONT bodyFont_ = nullptr;
        // 大号百分比：等宽字体，数字跳动时宽度不变，整行不会跟着晃
        HFONT bigFont_ = nullptr;
        HFONT monoFont_ = nullptr;
        HFONT monoSmFont_ = nullptr;

        ULONGLONG startTick_ = 0;

        static const wchar_t* WINDOW_CLASS_NAME;
        static constexpr UINT_PTR TIMER_ANIMATION = 1;
        static constexpr UINT WM_STATUS_CHANGED = WM_APP + 1;
        static constexpr UINT WM_REQUEST_QUIT = WM_APP + 2;
    };

} // namespace UnnamedBot
