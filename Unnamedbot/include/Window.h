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
    // 命中判定：入参为客户端坐标（物理像素），返回 true 表示该点为不透明内容。
    // 实现放在 AppCore：桌宠的命中源是渲染器读回的 alpha 掩码，
    // 快速模式下则是圆点的几何范围。
    using HitTestCallback = std::function<bool(int x, int y)>;

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

        // 设置逐像素命中判定源（为空时退回「整窗穿透」的旧行为）
        void SetHitTestProvider(HitTestCallback provider);

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

        // 将窗口吸附到屏幕右上角（主窗口以纯黑方块呈现）
        void SnapToTopRightOfScreen();

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

        // 以物理像素设置窗口位置，并夹取到工作区内。
        // 所有移动最终都收敛到这里：SetPosition（逻辑坐标）与拖拽（物理坐标）
        // 都走同一条路径，避免任何一条绕过夹取、把窗口留在屏幕外。
        void SetPositionPhysical(int x, int y);

        // 把物理像素坐标的窗口夹取到其所在显示器的工作区内。
        // 夹取的是窗口中心（可见内容的所在），因此桌宠可以贴到屏幕边角，
        // 但中心一定留在工作区内——「拖出屏幕后再也找不回来」不会发生。
        void ClampToWorkArea(int& x, int& y, int width, int height) const;

        // 窗口句柄
        HWND hwnd_ = nullptr;

        // 窗口类名
        static const wchar_t* WINDOW_CLASS_NAME;

        // 逻辑尺寸的合法范围：配置里的 window.width/height 可能被写成 0、负数
        // 或溢出值，而合成交换链要求尺寸必须为正，故统一在此收敛。
        static constexpr int MIN_WINDOW_SIZE = 64;
        static constexpr int MAX_WINDOW_SIZE = 4096;

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

        // 拖拽相关：按下时「抓手」在窗口内的偏移（物理像素），拖拽期间保持不变。
        // 窗口位置由「光标屏幕坐标 - 偏移」直接得出，绝不累加鼠标位移：
        // 旧实现用 dragWinX_（逻辑坐标）+ (pt.x - dragStartX_)（物理像素）再交给
        // SetPosition（又乘一次 dpiScale），等价于每帧做 w -> C - dpiScale*w 的迭代，
        // 增益为 -dpiScale，|增益| > 1 时是正反馈，窗口会在几十次鼠标事件内
        // 指数级飞离屏幕（日志实录：2801 -> 2241 -> ... 每次乘 1/1.25）。
        int dragGrabOffsetX_ = 0;
        int dragGrabOffsetY_ = 0;

        // 鼠标按下时间（毫秒）。用 ULONGLONG 与 GetTickCount64 同型：
        // 存进 DWORD 会在系统开机 49.7 天后回绕，拖拽阈值算错。
        ULONGLONG mouseDownTime_ = 0;
        bool isLeftDown_ = false;

        // 逐像素命中判定源：WM_NCHITTEST 每次都问它一次。
        // 它必须在 UI 线程上可重入且足够快（命中掩码查找是 O(1)）。
        HitTestCallback hitTestProvider_;

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