#include "../include/Window.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include <dwmapi.h>
#include <shellapi.h>
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
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = WINDOW_CLASS_NAME;

        // 如果已经注册，则无需重复注册
        if (FindWindowW(WINDOW_CLASS_NAME, nullptr)) {
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
        width_ = width;
        height_ = height;
        posX_ = x;
        posY_ = y;

        // 获取DPI缩放后的实际大小
        int scaledWidth = static_cast<int>(width * dpiScale_);
        int scaledHeight = static_cast<int>(height * dpiScale_);
        int scaledX = static_cast<int>(x * dpiScale_);
        int scaledY = static_cast<int>(y * dpiScale_);

        // 创建窗口：无边框、无标题栏、工具窗口
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            WINDOW_CLASS_NAME,
            title_.c_str(),
            WS_POPUP | WS_VISIBLE,
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

        visible_ = true;
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
        if (!hwnd_) {
            x = posX_;
            y = posY_;
            return;
        }
        RECT rect;
        GetWindowRect(hwnd_, &rect);
        x = static_cast<int>(rect.left / dpiScale_);
        y = static_cast<int>(rect.top / dpiScale_);
    }

    void Window::SetSize(int width, int height) {
        if (!hwnd_) return;
        width_ = width;
        height_ = height;
        int scaledWidth = static_cast<int>(width * dpiScale_);
        int scaledHeight = static_cast<int>(height * dpiScale_);
        SetWindowPos(hwnd_, nullptr, 0, 0, scaledWidth, scaledHeight, SWP_NOMOVE | SWP_NOZORDER);
    }

    void Window::GetSize(int& width, int& height) const {
        if (!hwnd_) {
            width = width_;
            height = height_;
            return;
        }
        RECT rect;
        GetWindowRect(hwnd_, &rect);
        width = static_cast<int>((rect.right - rect.left) / dpiScale_);
        height = static_cast<int>((rect.bottom - rect.top) / dpiScale_);
    }

    void Window::SetOpacity(float opacity) {
        if (!hwnd_) return;
        opacity_ = std::clamp(opacity, 0.2f, 1.0f);
        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(opacity_ * 255), LWA_ALPHA);
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
        return visible_ && hwnd_ != nullptr;
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
        // 启用 Per-Monitor V2 DPI感知
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            auto pSetProcessDPIAwarenessContext = reinterpret_cast<decltype(&SetProcessDPIAwarenessContext)>(
                GetProcAddress(hUser32, "SetProcessDPIAwarenessContext")
                );
            if (pSetProcessDPIAwarenessContext) {
                pSetProcessDPIAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
        int width = ConfigManager::Instance().GetInt("window.width", 400);
        int height = ConfigManager::Instance().GetInt("window.height", 400);
        bool topmost = ConfigManager::Instance().GetBool("window.always_on_top", true);
        float opacity = static_cast<float>(ConfigManager::Instance().GetDouble("window.opacity", 1.0));

        SetPosition(x, y);
        SetSize(width, height);
        SetAlwaysOnTop(topmost);
        SetOpacity(opacity);
        LOG_DEBUG("Window", "Window state restored.");
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

        case WM_MOUSEMOVE: {
            if (isLeftDown_ && GetCapture() == hwnd) {
                DWORD holdTime = GetTickCount64() - mouseDownTime_;
                if (holdTime >= 200) {
                    // 长按拖拽
                    isDragging_ = true;
                    POINT pt;
                    pt.x = GET_X_LPARAM(lParam);
                    pt.y = GET_Y_LPARAM(lParam);
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
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // 使用透明背景
            // 实际渲染由DX11完成，这里留空
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND: {
            // 返回1防止擦除背景（透明窗口）
            return 1;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

} // namespace UnnamedBot