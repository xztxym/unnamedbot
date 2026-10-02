#include "../include/QuickInputBar.h"

#include "../include/Logger.h"
#include "../include/Utils.h"

#include <CommCtrl.h>   // EM_SETCUEBANNER / EM_SETMARGINS

#include <algorithm>

namespace UnnamedBot {

    const wchar_t* QuickInputBar::WINDOW_CLASS_NAME = L"UnnamedBotQuickInputBar";
    WNDPROC QuickInputBar::previousEditProc_ = nullptr;

    namespace {

        // 逻辑像素（96 DPI）下的尺寸；实际建窗时按锚点所在显示器的 DPI 缩放。
        constexpr int kBarWidthLogical = 340;
        constexpr int kBarHeightLogical = 46;
        constexpr int kEditPadLogical = 10;
        constexpr int kGapLogical = 12;      // 与锚点窗口的间距
        constexpr int kEdgePadLogical = 8;   // 与工作区边缘的最小距离

        // 与配置终端同一套深色配色，避免弹出的小条看起来像另一个程序。
        const COLORREF kBackgroundColor = RGB(7, 13, 30);    // #070d1e
        const COLORREF kBorderColor = RGB(94, 124, 160);     // #5e7ca0
        const COLORREF kTextColor = RGB(232, 241, 255);      // #e8f1ff

        // 当前显示器 DPI：优先用 GetDpiForWindow（Win10 1607+），取不到再退回系统 DPI。
        int DpiForWindow(HWND hwnd) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            if (getDpiForWindow && hwnd) {
                const UINT dpi = getDpiForWindow(hwnd);
                if (dpi >= 72) return static_cast<int>(dpi);
            }
            HDC screen = GetDC(nullptr);
            const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(nullptr, screen);
            return dpi > 0 ? dpi : 96;
        }

    } // namespace

    QuickInputBar& QuickInputBar::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（与其它单例一致）。
        static QuickInputBar* instance = new QuickInputBar();
        return *instance;
    }

    bool QuickInputBar::Initialize(HINSTANCE hInstance) {
        instance_ = hInstance;
        return EnsureWindow();
    }

    void QuickInputBar::Shutdown() {
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        edit_ = nullptr;
        visible_ = false;
    }

    bool QuickInputBar::EnsureWindow() {
        if (hwnd_) {
            return true;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        // CS_DROPSHADOW：无标题栏的小窗口需要一点边界提示，否则在深色桌面上会「浮」不起来
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_ ? instance_ : GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
        wc.hbrBackground = nullptr;   // 背景在 WM_PAINT 里画，避免闪白
        wc.lpszClassName = WINDOW_CLASS_NAME;

        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(WNDCLASSEXW);
        if (!GetClassInfoExW(wc.hInstance, WINDOW_CLASS_NAME, &existing)) {
            if (!RegisterClassExW(&wc)) {
                LOG_ERROR("QuickInputBar", "Failed to register window class. Error: " + std::to_string(GetLastError()));
                return false;
            }
        }

        // 不做 WS_EX_NOACTIVATE：输入条必须能拿到键盘焦点（输入法候选窗也依赖它）。
        // WS_EX_TOOLWINDOW 让它不出现在任务栏与 Alt+Tab 里。
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            WINDOW_CLASS_NAME,
            L"",
            WS_POPUP,
            0, 0, 100, 100,
            nullptr, nullptr, wc.hInstance, this
        );
        if (!hwnd_) {
            LOG_ERROR("QuickInputBar", "Failed to create window. Error: " + std::to_string(GetLastError()));
            return false;
        }

        CreateEditControl();
        return true;
    }

    void QuickInputBar::CreateEditControl() {
        if (!hwnd_ || edit_) {
            return;
        }

        edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 10, 10,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)),
            instance_ ? instance_ : GetModuleHandle(nullptr),
            nullptr
        );
        if (!edit_) {
            LOG_ERROR("QuickInputBar", "Failed to create edit control. Error: " + std::to_string(GetLastError()));
            return;
        }

        // 子类化后由 EditProc 处理回车/ESC；原过程在成员里保存。
        previousEditProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickInputBar::EditProc)));

        // 空输入时的提示文字，省掉一个额外的静态文本控件。
        SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"说点什么…"));
        SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    }

    void QuickInputBar::Popup(HWND anchor, std::function<void(const std::string&)> onSubmit) {
        if (!EnsureWindow()) {
            return;
        }

        // 重复调用视为「重新开一次」：清掉上次残留的文本与回调，避免旧回调被再次触发。
        onSubmit_ = std::move(onSubmit);
        if (edit_) {
            SetWindowTextW(edit_, L"");
        }

        dpi_ = DpiForWindow(anchor ? anchor : hwnd_);
        const int width = kBarWidthLogical * dpi_ / 96;
        const int height = kBarHeightLogical * dpi_ / 96;

        int x = 0;
        int y = 0;
        ComputePosition(anchor, width, height, x, y);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);

        if (edit_) {
            const int pad = kEditPadLogical * dpi_ / 96;
            SetWindowPos(edit_, nullptr, pad, pad, width - pad * 2, height - pad * 2, SWP_NOZORDER);
            if (!font_) {
                // 中文用雅黑、西文回退到 Segoe UI：与配置终端保持一致的字体取向。
                font_ = CreateFontW(
                    -MulDiv(12, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            }
            if (font_) {
                SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            }
            SetFocus(edit_);
            // 光标定位到文本末尾（此时文本为空）
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        }

        visible_ = true;
        // 先提到前台再抢焦点：桌宠窗口是 TOOLWINDOW，不主动激活时输入条会拿不到键盘。
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, TRUE);
        LOG_DEBUG("QuickInputBar", "Popup at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    }

    void QuickInputBar::Close() {
        // 回调与文本都在这里结算：隐藏之后再有按键进来不应该还能提交。
        onSubmit_ = nullptr;
        if (edit_) {
            SetWindowTextW(edit_, L"");
        }
        if (hwnd_) {
            ShowWindow(hwnd_, SW_HIDE);
        }
        visible_ = false;
    }

    bool QuickInputBar::IsVisible() const {
        return hwnd_ && IsWindowVisible(hwnd_) != FALSE;
    }

    void QuickInputBar::ComputePosition(HWND anchor, int width, int height, int& x, int& y) const {
        const int gap = kGapLogical * dpi_ / 96;
        const int edge = kEdgePadLogical * dpi_ / 96;

        MONITORINFO info = {};
        info.cbSize = sizeof(MONITORINFO);
        HMONITOR monitor = anchor ? MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST)
                                  : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &info)) {
            info.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
        const RECT work = info.rcWork;
        // RECT 的成员是 LONG，与 int 是不同的类型；std::clamp 要求三个实参同类型，
        // 直接传 work.left 这类表达式会让模板推导失败（C2672），因此先取成 int 边界。
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);

        if (!anchor || !IsWindow(anchor)) {
            x = work.left + ((work.right - work.left) - width) / 2;
            y = work.top + ((work.bottom - work.top) - height) / 2;
            return;
        }

        RECT anchorRect = {};
        if (!GetWindowRect(anchor, &anchorRect)) {
            x = work.left + ((work.right - work.left) - width) / 2;
            y = work.top + ((work.bottom - work.top) - height) / 2;
            return;
        }

        // 水平方向与锚点居中对齐，并夹在工作区内
        x = anchorRect.left + ((anchorRect.right - anchorRect.left) - width) / 2;
        // 加括号调用 (std::max)：Windows.h 的 max 宏会破坏 std::max（本项目未定义 NOMINMAX）。
        x = std::clamp(x, workLeft + edge, (std::max)(workLeft + edge, workRight - width - edge));

        // 优先贴在锚点下方；下方放不下再翻到上方；都放不下就贴向空间较大的一侧。
        const int below = anchorRect.bottom + gap;
        const int above = anchorRect.top - gap - height;
        if (below + height <= work.bottom - edge) {
            y = below;
        }
        else if (above >= work.top + edge) {
            y = above;
        }
        else {
            const int spaceBelow = work.bottom - anchorRect.bottom;
            const int spaceAbove = anchorRect.top - work.top;
            y = (spaceBelow >= spaceAbove) ? (work.bottom - height - edge) : (work.top + edge);
        }
        y = std::clamp(y, workTop + edge, (std::max)(workTop + edge, workBottom - height - edge));
    }

    void QuickInputBar::Submit() {
        if (!edit_) {
            Close();
            return;
        }

        const int length = GetWindowTextLengthW(edit_);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(edit_, text.data(), length + 1);
        text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);

        const std::string utf8 = Utils::Trim(Utils::WideToUtf8(text));
        auto callback = onSubmit_;

        // 先收起输入条再回传：回调通常会发起一次网络请求，
        // 让输入条继续挂在屏幕上会让用户以为还没提交。
        Close();

        if (utf8.empty() || !callback) {
            return;
        }
        callback(utf8);
    }

    LRESULT CALLBACK QuickInputBar::EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN) {
            if (wParam == VK_RETURN) {
                Instance().Submit();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                Instance().Close();
                return 0;
            }
        }
        // 回车/ESC 的 WM_CHAR 不能再进控件：否则编辑框里会插入控制字符，还会响一声。
        // 退格等编辑键照常放行。
        if (msg == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
            return 0;
        }
        return CallWindowProcW(previousEditProc_, hwnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK QuickInputBar::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        QuickInputBar* bar = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            bar = reinterpret_cast<QuickInputBar*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bar));
        }
        else {
            bar = reinterpret_cast<QuickInputBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (bar) {
            return bar->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT QuickInputBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc = {};
            GetClientRect(hwnd, &rc);

            if (!backgroundBrush_) {
                backgroundBrush_ = CreateSolidBrush(kBackgroundColor);
            }
            FillRect(dc, &rc, backgroundBrush_);

            // 描边：与配置终端的冷冰蓝细边一致
            HPEN pen = CreatePen(PS_SOLID, 1, kBorderColor);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);

            EndPaint(hwnd, &ps);
            return 0;
        }

        // 输入框的底色与文字色：原生 EDIT 默认是白底黑字，与深色输入条格格不入。
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kTextColor);
            SetBkColor(dc, kBackgroundColor);
            if (!backgroundBrush_) {
                backgroundBrush_ = CreateSolidBrush(kBackgroundColor);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }

        // 点了别处就收起：输入条是无边框浮层，必须有「点外部关闭」这个出口。
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                Close();
            }
            return 0;
        }

        case WM_CLOSE: {
            Close();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY: {
            hwnd_ = nullptr;
            edit_ = nullptr;
            visible_ = false;
            return 0;
        }

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace UnnamedBot
