#pragma once

#include <Windows.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>

namespace UnnamedBot {

    // 窗口事件回调函数类型
    using WindowEventCallback = std::function<void()>;
    using MouseEventCallback = std::function<void(int x, int y, bool isLeftDown)>;
    using DragEventCallback = std::function<void(int x, int y)>;

    // 窗口管理类
    class Window {
    public:
        Window();
        ~Window();

        // 创建窗口
        bool Create(const std::wstring& title = L"UnnamedBot",
            int width = 400,
            int height = 400,
            int x = 100,
            int y = 100);

        // 销毁窗口
        void Destroy();

        // 显示/隐藏窗口
        void Show(bool show = true);
        void Hide();

        // 设置窗口位置
        void SetPosition(int x, int y);

        // 获取窗口位置
        void GetPosition(int& x, int& y) const;

        // 设置窗口大小
        void SetSize(int width, int height);

        // 获取窗口大小
        void GetSize(int& width, int& height) const;

        // 设置透明度 (0.0 ~ 1.0)
        void SetOpacity(float opacity);

        // 设置置顶模式
        void SetAlwaysOnTop(bool topmost);

        // 设置点击穿透
        void SetClickThrough(bool enabled);

        // 设置鼠标悬停光标
        void SetHoverCursor(bool isHand);

        // 消息循环（阻塞）
        void RunMessageLoop();

        // 发送退出消息（结束消息循环）
        void PostQuitMessage();

        // 获取窗口句柄
        HWND GetHandle() const { return hwnd_; }

        // 设置事件回调
        void OnWindowClose(WindowEventCallback callback);
        void OnWindowMove(DragEventCallback callback);
        void OnWindowResize(std::function<void(int width, int height)> callback);
        void OnMouseClick(MouseEventCallback callback);
        void OnMouseDrag(DragEventCallback callback);
        void OnMouseHover(std::function<void(int x, int y)> callback);

        // 检查窗口是否可见
        bool IsVisible() const;

        // 检查是否处于置顶模式
        bool IsAlwaysOnTop() const;

        // 检测全屏应用（用于自动隐藏）
        bool IsFullscreenAppRunning() const;

        // 获取DPI缩放因子
        float GetDPIScale() const;

        // 设置DPI感知
        void SetDPIAwareness();

        // 保存窗口位置和状态到配置
        void SaveState();

        // 从配置恢复窗口位置和状态
        void RestoreState();

    private:
        // 窗口过程（静态）
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 窗口过程（非静态）
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 注册窗口类
        bool RegisterWindowClass();

        // 更新窗口样式
        void UpdateWindowStyles();

        // 处理鼠标点击
        void HandleMouseClick(int x, int y, bool isLeftDown);

        // 处理鼠标拖拽
        void HandleMouseDrag(int x, int y);

        // 检查是否为鼠标穿透区域（基于Live2D像素检测，这里由外部决定）
        bool IsPixelTransparent(int x, int y);

        // 获取系统DPI缩放
        float GetSystemDPIScale();

        // 窗口句柄
        HWND hwnd_ = nullptr;

        // 窗口类名
        static const wchar_t* WINDOW_CLASS_NAME;

        // 窗口属性
        std::wstring title_;
        int width_ = 400;
        int height_ = 400;
        int posX_ = 100;
        int posY_ = 100;
        float opacity_ = 1.0f;
        bool alwaysOnTop_ = true;
        bool clickThrough_ = true;
        bool visible_ = false;
        bool isDragging_ = false;
        bool isHovering_ = false;

        // DPI缩放
        float dpiScale_ = 1.0f;

        // 拖拽相关
        int dragStartX_ = 0;
        int dragStartY_ = 0;
        int dragWinX_ = 0;
        int dragWinY_ = 0;

        // 鼠标按下时间（毫秒）
        DWORD mouseDownTime_ = 0;
        bool isLeftDown_ = false;

        // 回调函数
        WindowEventCallback onClose_;
        DragEventCallback onMove_;
        std::function<void(int width, int height)> onResize_;
        MouseEventCallback onMouseClick_;
        DragEventCallback onMouseDrag_;
        std::function<void(int x, int y)> onMouseHover_;

        // 线程安全
        std::atomic<bool> isRunning_ = false;

        // 是否使用像素级穿透检测
        bool usePixelHitTest_ = true;
    };

} // namespace UnnamedBot