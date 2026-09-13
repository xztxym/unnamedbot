#include "../include/Window.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace UnnamedBot {

    const wchar_t* Window::WINDOW_CLASS_NAME = L"UnnamedBotWindowClass";

    Window::Window() {
        // 注册窗口类（在构造函数中注册，避免重复注册）
        RegisterWindowClass();
        // 初始化DPI感知
        SetDPIAwareness();
        dpiScale_ = GetSystemDPIScale();
    }

    Window::~Window() {
        Destroy();
    }

    bool Window::RegisterWindowClass() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        // 不用背景刷：窗口以 WS_EX_NOREDIRECTIONBITMAP 创建，内容全部由
        // DirectComposition 呈现。若在这里铺黑刷，渲染失败时就会退化成
        // 一个黑色方块（正是要修掉的现象）。
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;

        // 只检查本模块是否已注册该窗口类。不能用 FindWindowW 探测：
        // 它按类名在整个系统内查找，别的进程注册同名类时会让本进程跳过注册，
        // 随后 CreateWindowExW 直接失败（错误 1407）。
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(WNDCLASSEXW);
        if (GetClassInfoExW(wc.hInstance, WINDOW_CLASS_NAME, &existing)) {
            return true;
        }

        if (!RegisterClassExW(&wc)) {
            LOG_ERROR("Window", "Failed to register window class. Error: " + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    bool Window::Create(const std::wstring& title, int width, int height, int x, int y) {
        if (hwnd_) {
            LOG_WARN("Window", "Window already created.");
            return true;
        }

        title_ = title;
        // 配置里的尺寸不可信（历史版本可能写入过垃圾值），先收敛到合法范围。
        width_ = std::clamp(width, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        height_ = std::clamp(height, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        posX_ = x;
        posY_ = y;

        // 获取DPI缩放后的实际大小
        int scaledWidth = static_cast<int>(width_ * dpiScale_);
        int scaledHeight = static_cast<int>(height_ * dpiScale_);
        int scaledX = static_cast<int>(x * dpiScale_);
        int scaledY = static_cast<int>(y * dpiScale_);

        // 创建窗口：无边框、无标题栏、工具窗口。
        // 使用 WS_EX_NOREDIRECTIONBITMAP 而非 WS_EX_LAYERED：窗口内容完全由
        // DirectComposition 呈现，这样 backbuffer 的 alpha 才能实现逐像素透明，
        // 而不是整窗铺一层黑底（WS_EX_LAYERED 只能做整体 alpha）。
        hwnd_ = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            WINDOW_CLASS_NAME,
            title_.c_str(),
            WS_POPUP,
            scaledX, scaledY,
            scaledWidth, scaledHeight,
            nullptr,
            nullptr,
            GetModuleHandle(nullptr),
            this
        );

        if (!hwnd_) {
            LOG_ERROR("Window", "Failed to create window. Error: " + std::to_string(GetLastError()));
            return false;
        }

        // 设置透明度
        SetOpacity(opacity_);

        // 应用配置
        alwaysOnTop_ = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_ALWAYS_ON_TOP, true);
        clickThrough_ = ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_CLICK_THROUGH, true);
        SetAlwaysOnTop(alwaysOnTop_);
        SetClickThrough(clickThrough_);

        // 创建时不显示：由调用方（AppCore）在用户确认后才显示桌宠
        visible_ = false;
        isRunning_ = true;

        LOG_INFO("Window", "Window created successfully.");
        return true;
    }

    void Window::Destroy() {
        if (hwnd_) {
            LOG_INFO("Window", "Destroying window...");
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            isRunning_ = false;
            visible_ = false;
        }
    }

    void Window::Show(bool show) {
        if (!hwnd_) return;
        if (show) {
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            visible_ = true;
        }
        else {
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
        }
    }

    void Window::Hide() {
        Show(false);
    }

    void Window::SetPosition(int x, int y) {
        if (!hwnd_) return;
        posX_ = x;
        posY_ = y;
        int scaledX = static_cast<int>(x * dpiScale_);
        int scaledY = static_cast<int>(y * dpiScale_);
        SetWindowPos(hwnd_, nullptr, scaledX, scaledY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    void Window::GetPosition(int& x, int& y) const {
        if (!hwnd_ || dpiScale_ <= 0.0f) {
            x = posX_;
            y = posY_;
            return;
        }
        RECT rect = {};
        if (!GetWindowRect(hwnd_, &rect)) {
            // 取不到窗口矩形时退回缓存的逻辑坐标，不要把未初始化的值当成坐标。
            x = posX_;
            y = posY_;
            return;
        }
        x = static_cast<int>(rect.left / dpiScale_);
        y = static_cast<int>(rect.top / dpiScale_);
    }

    void Window::SetSize(int width, int height) {
        if (!hwnd_) return;
        width_ = std::clamp(width, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        height_ = std::clamp(height, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        int scaledWidth = static_cast<int>(width_ * dpiScale_);
        int scaledHeight = static_cast<int>(height_ * dpiScale_);
        SetWindowPos(hwnd_, nullptr, 0, 0, scaledWidth, scaledHeight, SWP_NOMOVE | SWP_NOZORDER);
    }

    void Window::GetSize(int& width, int& height) const {
        if (!hwnd_ || dpiScale_ <= 0.0f) {
            width = width_;
            height = height_;
            return;
        }
        // RECT 必须先初始化：GetWindowRect 失败（例如窗口已销毁）时它不会被写入，
        // 曾因此把未初始化的窗口矩形存进配置，使 window.width/height 变成垃圾值，
        // 最终导致合成交换链以非法尺寸创建、程序在显示窗口前就退出。
        RECT rect = {};
        if (!GetWindowRect(hwnd_, &rect)) {
            width = width_;
            height = height_;
            return;
        }
        width = std::clamp(static_cast<int>((rect.right - rect.left) / dpiScale_),
            MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        height = std::clamp(static_cast<int>((rect.bottom - rect.top) / dpiScale_),
            MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
    }

    void Window::SetOpacity(float opacity) {
        opacity_ = std::clamp(opacity, 0.2f, 1.0f);
        // 窗口改用 DirectComposition 呈现（WS_EX_NOREDIRECTIONBITMAP），
        // LWA_ALPHA 仅对分层窗口有效且会给整窗铺黑底，这里不再使用；
        // 实际整窗透明度由 Renderer::SetWindowOpacity 通过合成视觉施加。
    }

    void Window::SetAlwaysOnTop(bool topmost) {
        if (!hwnd_) return;
        alwaysOnTop_ = topmost;
        SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        LOG_DEBUG("Window", "Always on top: " + std::string(topmost ? "enabled" : "disabled"));
    }

    void Window::SetClickThrough(bool enabled) {
        if (!hwnd_) return;
        clickThrough_ = enabled;
        // 修改窗口样式
        LONG exStyle = GetWindowLong(hwnd_, GWL_EXSTYLE);
        if (enabled) {
            exStyle |= WS_EX_TRANSPARENT;
        }
        else {
            exStyle &= ~WS_EX_TRANSPARENT;
        }
        SetWindowLong(hwnd_, GWL_EXSTYLE, exStyle);
        // 刷新窗口
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        LOG_DEBUG("Window", "Click through: " + std::string(enabled ? "enabled" : "disabled"));
    }

    void Window::SetHoverCursor(bool isHand) {
        if (!hwnd_) return;
        HCURSOR cursor = isHand ? LoadCursor(nullptr, IDC_HAND) : LoadCursor(nullptr, IDC_ARROW);
        SetCursor(cursor);
    }

    void Window::RunMessageLoop() {
        if (!hwnd_) {
            LOG_ERROR("Window", "Cannot run message loop: window not created.");
            return;
        }

        MSG msg = {};
        while (isRunning_ && GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void Window::PostQuitMessage() {
        isRunning_ = false;
        ::PostQuitMessage(0);
    }

    void Window::OnWindowClose(WindowEventCallback callback) {
        onClose_ = callback;
    }

    void Window::OnWindowMove(DragEventCallback callback) {
        onMove_ = callback;
    }

    void Window::OnWindowResize(std::function<void(int width, int height)> callback) {
        onResize_ = callback;
    }

    void Window::OnMouseClick(MouseEventCallback callback) {
        onMouseClick_ = callback;
    }

    void Window::OnMouseDrag(DragEventCallback callback) {
        onMouseDrag_ = callback;
    }

    void Window::OnMouseHover(std::function<void(int x, int y)> callback) {
        onMouseHover_ = callback;
    }

    bool Window::IsVisible() const {
        // 以窗口真实状态为准：窗口也可能被系统或外部调用显示/隐藏，
        // 仅缓存 visible_ 会与屏幕上的实际状态脱节（渲染循环按此决定是否绘制）。
        if (!hwnd_) {
            return false;
        }
        return IsWindowVisible(hwnd_) != FALSE;
    }

    bool Window::IsAlwaysOnTop() const {
        return alwaysOnTop_;
    }

    bool Window::IsFullscreenAppRunning() const {
        // 检测是否有全屏应用运行（简化版）
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return false;
        RECT rect;
        GetWindowRect(hwnd, &rect);
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        // 如果窗口覆盖整个屏幕，且不是自己的窗口，视为全屏应用
        if (rect.left <= 0 && rect.top <= 0 && rect.right >= screenWidth && rect.bottom >= screenHeight) {
            wchar_t className[256];
            GetClassNameW(hwnd, className, 256);
            // 忽略任务栏等系统窗口
            if (wcscmp(className, L"Shell_TrayWnd") != 0 && wcscmp(className, L"Button") != 0) {
                return true;
            }
        }
        return false;
    }

    float Window::GetDPIScale() const {
        return dpiScale_;
    }

    void Window::SetDPIAwareness() {
        // 启用 Per-Monitor V2 DPI感知（动态加载以兼容旧系统）
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            auto pSetProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")
                );
            if (pSetProcessDpiAwarenessContext) {
                pSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                LOG_DEBUG("Window", "DPI awareness set to Per-Monitor V2");
            }
        }
    }

    float Window::GetSystemDPIScale() {
        HDC hdc = GetDC(nullptr);
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        return static_cast<float>(dpiX) / 96.0f;
    }

    void Window::SaveState() {
        // 保存窗口位置、大小等配置
        int x, y;
        GetPosition(x, y);
        ConfigManager::Instance().SetInt("window.position_x", x);
        ConfigManager::Instance().SetInt("window.position_y", y);
        int width, height;
        GetSize(width, height);
        ConfigManager::Instance().SetInt("window.width", width);
        ConfigManager::Instance().SetInt("window.height", height);
        ConfigManager::Instance().SetBool("window.always_on_top", alwaysOnTop_);
        ConfigManager::Instance().SetDouble("window.opacity", opacity_);
        LOG_DEBUG("Window", "Window state saved.");
    }

    void Window::RestoreState() {
        // 从配置恢复窗口状态
        int x = ConfigManager::Instance().GetInt("window.position_x", 100);
        int y = ConfigManager::Instance().GetInt("window.position_y", 100);
        // 配置里的尺寸不可信（历史版本可能写入过垃圾值），应用前先收敛。
        int width = std::clamp(ConfigManager::Instance().GetInt("window.width", 400),
            MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        int height = std::clamp(ConfigManager::Instance().GetInt("window.height", 400),
            MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        bool topmost = ConfigManager::Instance().GetBool("window.always_on_top", true);
        float opacity = static_cast<float>(ConfigManager::Instance().GetDouble("window.opacity", 1.0));

        SetPosition(x, y);
        SetSize(width, height);
        SetAlwaysOnTop(topmost);
        SetOpacity(opacity);
        LOG_DEBUG("Window", "Window state restored.");
    }

    void Window::SnapToTopRightOfScreen() {
        if (!hwnd_) return;

        // 进程启用了 Per-Monitor V2 DPI 感知，这里的坐标都是物理像素
        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(monitor, &info)) {
            LOG_WARN("Window", "Failed to query monitor info.");
            return;
        }

        RECT rect = {};
        GetWindowRect(hwnd_, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        // 贴住工作区右上角
        int x = info.rcWork.right - width;
        if (x < info.rcWork.left) x = info.rcWork.left;
        const int y = info.rcWork.top;

        SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

        // 同步逻辑坐标，供 GetPosition/SaveState 使用
        posX_ = static_cast<int>(x / dpiScale_);
        posY_ = static_cast<int>(y / dpiScale_);

        LOG_INFO("Window", "Snapped to top-right corner (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    }

    // 静态窗口过程
    LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Window* pWindow = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pWindow = reinterpret_cast<Window*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWindow));
        }
        else {
            pWindow = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pWindow) {
            return pWindow->HandleMessage(hwnd, msg, wParam, lParam);
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // 实例窗口过程
    LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_NCHITTEST: {
            // 点击穿透：返回HTTRANSPARENT让鼠标穿透
            if (clickThrough_) {
                // 如果需要像素级检测，这里应该获取鼠标位置，查询Live2D是否命中
                // 如果命中，返回HTCLIENT，否则返回HTTRANSPARENT
                // 但此处简化：直接返回HTTRANSPARENT，由外部通过IsHit检测
                // 若启用像素检测，则在这里调用外部接口
                if (usePixelHitTest_) {
                    // 获取鼠标位置
                    POINT pt;
                    pt.x = GET_X_LPARAM(lParam);
                    pt.y = GET_Y_LPARAM(lParam);
                    ScreenToClient(hwnd, &pt);
                    // 这里应该调用Live2D的IsHit检测
                    // 暂时返回HTTRANSPARENT允许穿透
                    return HTTRANSPARENT;
                }
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            if (onMouseHover_) {
                onMouseHover_(pt.x, pt.y);
            }
            // 更新光标（如果需要）
            if (clickThrough_) {
                // 检测是否在模型区域，若是则显示手型光标
                // 简化：显示默认箭头
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
            }
            // 长按拖拽
            if (isLeftDown_ && GetCapture() == hwnd) {
                DWORD holdTime = GetTickCount64() - mouseDownTime_;
                if (holdTime >= 200) {
                    isDragging_ = true;
                    // 计算新位置
                    int deltaX = pt.x - dragStartX_;
                    int deltaY = pt.y - dragStartY_;
                    int newX = dragWinX_ + deltaX;
                    int newY = dragWinY_ + deltaY;
                    SetPosition(newX, newY);
                    if (onMouseDrag_) {
                        onMouseDrag_(newX, newY);
                    }
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            mouseDownTime_ = GetTickCount64();
            isLeftDown_ = true;
            // 记录拖拽起始位置
            dragStartX_ = pt.x;
            dragStartY_ = pt.y;
            GetPosition(dragWinX_, dragWinY_);
            // 如果是点击非穿透区域，可以在这里处理
            if (!clickThrough_) {
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            isLeftDown_ = false;
            DWORD holdTime = GetTickCount64() - mouseDownTime_;
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            // 判断是单击还是长按拖拽
            if (holdTime < 200) {
                // 单击
                if (onMouseClick_) {
                    onMouseClick_(pt.x, pt.y, true);
                }
            }
            else {
                // 长按拖拽结束
                if (isDragging_) {
                    isDragging_ = false;
                    // 保存位置
                    SaveState();
                }
            }
            if (!clickThrough_) {
                ReleaseCapture();
            }
            return 0;
        }

        case WM_RBUTTONDOWN: {
            // 右键点击
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            if (onMouseClick_) {
                onMouseClick_(pt.x, pt.y, false);
            }
            return 0;
        }

        case WM_SIZE: {
            if (onResize_) {
                int newWidth = LOWORD(lParam);
                int newHeight = HIWORD(lParam);
                onResize_(newWidth, newHeight);
            }
            return 0;
        }

        case WM_MOVE: {
            if (onMove_) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                onMove_(x, y);
            }
            return 0;
        }

        case WM_DESTROY: {
            isRunning_ = false;
            if (onClose_) {
                onClose_();
            }
            PostQuitMessage();
            return 0;
        }

        case WM_PAINT: {
            // 窗口内容完全由 DirectComposition 呈现（WS_EX_NOREDIRECTIONBITMAP），
            // 这里绝不能自己画任何东西：此前在此处铺黑刷，正是“桌面上只有一个
            // 黑方块”的来源。BeginPaint/EndPaint 成对调用只是确认重绘区域。
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            // 同理不擦背景，返回 1 表示已处理（否则系统会再刷一次底色）。
            return 1;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

} // namespace UnnamedBot