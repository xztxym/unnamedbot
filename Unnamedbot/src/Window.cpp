#include "../include/Window.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
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
        //
        // 刻意不带 WS_EX_TRANSPARENT：那个样式会让系统在命中测试阶段就跳过本窗口，
        // WM_NCHITTEST 根本不会送到这里，鼠标一律落到下层窗口。
        // 穿透改由 WM_NCHITTEST 按需返回 HTTRANSPARENT 实现（见 HandleMessage），
        // 这样模型/圆点才点得到。
        hwnd_ = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
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
        // 配置里的位置历史上可能被写成天文数字，先收进一个远大于任何多屏桌面的
        // 范围再做 dpi 乘法：否则 x * dpiScale 会在转成 int 前溢出。
        constexpr int kSaneLimit = 1000000;
        x = std::clamp(x, -kSaneLimit, kSaneLimit);
        y = std::clamp(y, -kSaneLimit, kSaneLimit);
        // 入参是逻辑坐标（与 GetPosition/SaveState 同一坐标系）。
        const float scale = (dpiScale_ > 0.0f) ? dpiScale_ : 1.0f;
        SetPositionPhysical(static_cast<int>(std::lround(x * scale)),
            static_cast<int>(std::lround(y * scale)));
    }

    void Window::SetPositionPhysical(int x, int y) {
        if (!hwnd_) return;

        // 先量窗口的物理尺寸：夹取需要它，且必须在移动之前取到（移动不改尺寸）。
        RECT rect = {};
        int width = 0;
        int height = 0;
        if (GetWindowRect(hwnd_, &rect)) {
            width = static_cast<int>(rect.right - rect.left);
            height = static_cast<int>(rect.bottom - rect.top);
        }

        // 夹取：越界的位置（拖出屏幕、配置里的历史垃圾值）一律拉回工作区。
        // 这是「圆点还能找回来」的兜底——只靠拖拽算对还不够，任何写入路径
        // （配置恢复、托盘菜单）都可能送进来一个屏幕外的坐标。
        ClampToWorkArea(x, y, width, height);

        if (!SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER)) {
            LOG_WARN("Window", "SetWindowPos failed. Error: " + std::to_string(GetLastError()));
        }

        // 同步逻辑坐标缓存，供 GetPosition 在取不到窗口矩形时兜底。
        const float scale = (dpiScale_ > 0.0f) ? dpiScale_ : 1.0f;
        posX_ = static_cast<int>(std::lround(x / scale));
        posY_ = static_cast<int>(std::lround(y / scale));
    }

    void Window::ClampToWorkArea(int& x, int& y, int width, int height) const {
        // 夹取的对象是「窗口中心」，不是整个窗口矩形。
        // 桌宠窗口绝大部分是透明的：看得见、点得到的内容（快速模式的圆点、
        // 普通模式的人物）都落在中心附近。若要求整窗可见，可见内容就永远
        // 离屏幕边缘差半个窗口（400 逻辑窗口在 125% 下是 250 物理像素），
        // 「把桌宠拖到屏幕边角」这种最平常的用法会直接失效。
        // 保证中心在工作区内 = 保证看得见的东西一定还在屏幕里，
        // 无论从哪条路径写进来的坐标都不会让桌宠「失踪」。
        RECT probe = { x, y, x + width, y + height };
        MONITORINFO info = {};
        info.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST), &info)) {
            LOG_WARN("Window", "Failed to query monitor work area; position left unclamped.");
            return;
        }

        // RECT 成员是 LONG，与 int 不同型，std::clamp 要求三者同型，先取成 int。
        const int workLeft = static_cast<int>(info.rcWork.left);
        const int workTop = static_cast<int>(info.rcWork.top);
        const int workRight = static_cast<int>(info.rcWork.right);
        const int workBottom = static_cast<int>(info.rcWork.bottom);

        // 留一点边距：圆点在 125% 缩放下半径约 15 物理像素，
        // 不留边距时把中心拖到工作区角上会把圆点切掉大半。
        constexpr int kContentEdgeMargin = 24;
        const int minCenterX = workLeft + kContentEdgeMargin;
        const int maxCenterX = (workRight - kContentEdgeMargin > minCenterX)
            ? workRight - kContentEdgeMargin : minCenterX;
        const int minCenterY = workTop + kContentEdgeMargin;
        const int maxCenterY = (workBottom - kContentEdgeMargin > minCenterY)
            ? workBottom - kContentEdgeMargin : minCenterY;

        // width/height 为 0（取不到窗口矩形）时退化为夹取左上角，同样安全。
        const int centerX = x + width / 2;
        const int centerY = y + height / 2;
        x += std::clamp(centerX, minCenterX, maxCenterX) - centerX;
        y += std::clamp(centerY, minCenterY, maxCenterY) - centerY;
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
        // 不再用 WS_EX_TRANSPARENT 实现穿透：该样式会让系统跳过本窗口的命中测试，
        // 连 WM_NCHITTEST 都不会送到这里，像素级判定就无从谈起（也正是
        // 「左键无法拖动、右键没反应」的根因）。现在的做法是始终接收命中测试，
        // 由 WM_NCHITTEST 决定穿透还是不穿透。
        LOG_DEBUG("Window", "Click through: " + std::string(enabled ? "enabled" : "disabled"));
    }

    void Window::SetHitTestProvider(HitTestCallback provider) {
        hitTestProvider_ = std::move(provider);
    }

    bool Window::IsPixelTransparent(int x, int y) {
        // 没有判定源时视为整窗透明：维持「不做像素检测就全穿透」的旧语义。
        if (!hitTestProvider_) {
            return true;
        }
        return !hitTestProvider_(x, y);
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
            // 关闭穿透：整窗接收鼠标（“抓住窗口任何位置”的旧行为）
            if (!clickThrough_) {
                return HTCLIENT;
            }
            // 命中判定：只有不透明内容（模型 / 圆点）参与点击，其余地方把鼠标让给下层窗口。
            // 这是「碰撞框只覆盖人物」的唯一实现点；像素掩码由渲染线程每帧读回。
            if (usePixelHitTest_ && hitTestProvider_) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                return IsPixelTransparent(pt.x, pt.y) ? HTTRANSPARENT : HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        case WM_MOUSEMOVE: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            if (onMouseHover_) {
                onMouseHover_(pt.x, pt.y);
            }
            // 悬停在可点区域上时给个手型：否则一个透明的窗口「这里能点吗」全靠试
            if (usePixelHitTest_ && hitTestProvider_) {
                SetCursor(LoadCursor(nullptr, IsPixelTransparent(pt.x, pt.y) ? IDC_ARROW : IDC_HAND));
            }

            // 长按拖拽：按住超过阈值才算拖动，否则松开时是「单击」
            if (isLeftDown_ && GetCapture() == hwnd && !isDragging_) {
                constexpr DWORD kDragHoldMilliseconds = 200;
                if (GetTickCount64() - mouseDownTime_ >= kDragHoldMilliseconds) {
                    isDragging_ = true;
                }
            }
            if (isDragging_) {
                // 拖动是行为开关，不是状态：配置里关掉时立即生效，
                // 不需要重启程序（配置页保存后会重载）
                if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_FEATURE_DRAG_ENABLED, true)) {
                    isDragging_ = false;
                }
                else {
                    // 用光标的屏幕坐标（物理像素）反推窗口左上角：
                    // 位置只由光标与固定的抓手偏移决定，不依赖窗口当前在哪，
                    // 因此窗口被移动后不会反过来改变下一次的计算结果
                    // （旧写法用客户端坐标累加位移，窗口一移动客户端坐标就变，
                    // 形成正反馈把窗口甩出屏幕）。
                    POINT cursor = {};
                    if (GetCursorPos(&cursor)) {
                        const int newX = static_cast<int>(cursor.x) - dragGrabOffsetX_;
                        const int newY = static_cast<int>(cursor.y) - dragGrabOffsetY_;
                        SetPositionPhysical(newX, newY);
                        if (onMouseDrag_) {
                            // 回调沿用逻辑坐标（与 GetPosition 一致）
                            onMouseDrag_(posX_, posY_);
                        }
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
            isDragging_ = false;
            // 记录「抓手」在窗口内的偏移（物理像素）：光标屏幕坐标 - 窗口左上角。
            // 拖拽期间它是常量，窗口位置 = 光标位置 - 偏移。
            dragGrabOffsetX_ = pt.x;
            dragGrabOffsetY_ = pt.y;
            {
                POINT cursor = {};
                RECT windowRect = {};
                if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &windowRect)) {
                    dragGrabOffsetX_ = static_cast<int>(cursor.x - windowRect.left);
                    dragGrabOffsetY_ = static_cast<int>(cursor.y - windowRect.top);
                }
                // 取不到时退回客户端坐标：此时窗口未被移动过，两者等价。
            }
            // 无条件捕获鼠标：拖动过程中光标会离开窗口（甚至跑到其他窗口上方），
            // 不捕获就只能收到窗口内的移动，拖到一半就断。
            // （旧实现只在「关闭穿透」时捕获，而默认穿透，因此永远拖不动。）
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            isLeftDown_ = false;
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            const bool dragged = isDragging_;
            isDragging_ = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (dragged) {
                // 拖动结束：把新位置写回配置，下次启动还在同一处
                SaveState();
            }
            else if (onMouseClick_) {
                // 未拖动 → 单击（是否会真的触发由接收方按命中判定决定）
                onMouseClick_(pt.x, pt.y, true);
            }
            return 0;
        }

        // 右键菜单在「拾起」时弹出：菜单跟着鼠标弹起会把自己的点击吃掉，
        // 而 Windows 的惯例也是右键抬起时出菜单。
        case WM_RBUTTONDOWN: {
            return 0;
        }

        case WM_RBUTTONUP: {
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
                // WM_SIZE 给的是物理像素，而渲染缓冲、命中掩码与 Window::GetSize
                // 全部按逻辑像素维护；这里不换算的话，一旦真的发生尺寸变化，
                // 缓冲尺寸会和客户端鼠标坐标差一个 dpi 因子，命中判定整体偏移。
                const float scale = (dpiScale_ > 0.0f) ? dpiScale_ : 1.0f;
                const int newWidth = static_cast<int>(LOWORD(lParam) / scale);
                const int newHeight = static_cast<int>(HIWORD(lParam) / scale);
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