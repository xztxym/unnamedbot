#include "../include/QuickInputBar.h"

#include "../include/Logger.h"
#include "../include/UiTheme.h"
#include "../include/Utils.h"

#include <CommCtrl.h>   // EM_SETCUEBANNER / EM_SETMARGINS
#include <Imm.h>        // 组字串（GCS_COMPSTR）

#include <algorithm>

#pragma comment(lib, "imm32.lib")

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

        // 下划线式：整体更矮，下沿一条线就是全部装饰
        constexpr int kUnderlineHeightLogical = 34;
        constexpr int kUnderlineThicknessLogical = 2;
        constexpr int kUnderlineInsetLogical = 2;

        // 色键：整窗铺这个颜色，系统把该颜色的像素全部抠成透明。
        // 可见内容（正文色、主题色）都不是纯黑，所以不会误抠到字和线。
        const COLORREF kTransparentKey = RGB(0, 0, 0);

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
            backgroundBrushColor_ = CLR_INVALID;
        }
        if (keyBrush_) {
            DeleteObject(keyBrush_);
            keyBrush_ = nullptr;
            keyBrushColor_ = CLR_INVALID;
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
        Show(anchor, std::move(onSubmit), Style::Panel);
    }

    void QuickInputBar::PopupUnderline(HWND anchor, std::function<void(const std::string&)> onSubmit) {
        Show(anchor, std::move(onSubmit), Style::Underline);
    }

    HBRUSH QuickInputBar::EnsureBrush(HBRUSH& brush, COLORREF& cached, COLORREF color) {
        if (!brush || cached != color) {
            if (brush) {
                DeleteObject(brush);
            }
            brush = CreateSolidBrush(color);
            cached = color;
        }
        return brush;
    }

    namespace {

        // 取 EDIT 当前文本
        std::wstring ReadWindowText(HWND hwnd) {
            const int length = GetWindowTextLengthW(hwnd);
            if (length <= 0) {
                return std::wstring();
            }
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
            text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
            return text;
        }

        // 取输入法正在组字、但还没上屏的内容（没有则返回空）。
        // 组字期间 EDIT 不会发 EN_CHANGE，所以这段文字只能主动去问输入法。
        std::wstring ReadComposition(HWND edit) {
            std::wstring result;
            HIMC context = ImmGetContext(edit);
            if (!context) {
                return result;
            }
            const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
            if (bytes > 0) {
                std::wstring buffer(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
                const LONG copied = ImmGetCompositionStringW(context, GCS_COMPSTR, buffer.data(), bytes);
                if (copied > 0) {
                    result.assign(buffer.data(), static_cast<size_t>(copied) / sizeof(wchar_t));
                }
            }
            ImmReleaseContext(edit, context);
            return result;
        }

    } // namespace

    // 下划线式专用：把 EDIT 的文本（含未上屏的组字串）画到父窗口的 DC 上。
    // 输入、输入法、选区仍由原生 EDIT 负责；这里只负责显示，所以光标也自己画。
    void QuickInputBar::DrawEditContent(HDC dc, const UiTheme& theme) {
        if (!edit_) {
            return;
        }

        // 文字区固定取「下划线上方那一块」：EDIT 已经被挪去做纯输入接收器，
        // 不能再拿它的位置当排版依据（否则文字会被画到窗口外的 1x1 区域里）。
        RECT rc = {};
        GetClientRect(hwnd_, &rc);
        const int pad = kEditPadLogical * dpi_ / 96;
        const int lineZone = (kUnderlineThicknessLogical + kUnderlineInsetLogical) * dpi_ / 96;
        RECT textRect = { rc.left + pad, rc.top, rc.right - pad, rc.bottom - lineZone };

        const std::wstring text = ReadWindowText(edit_);
        const std::wstring composing = ReadComposition(edit_);
        HFONT font = EnsureFont(theme);

        if (text.empty() && composing.empty()) {
            // 提示文字由我们自己画：EDIT 的 cue banner 在分层窗口里同样显示不出来。
            // 先画光标（贴在最左边），提示文字让开它，省得叠在一起。
            DrawCaret(dc, theme, textRect, 0);

            HGDIOBJ placeholderFont = font ? SelectObject(dc, font) : nullptr;
            const int placeholderMode = SetBkMode(dc, TRANSPARENT);
            const COLORREF placeholderColor = SetTextColor(dc, theme.textDim);
            const int caretWidth = (std::max)(1, kUnderlineThicknessLogical * dpi_ / 96 / 2);
            RECT placeholderRect = textRect;
            placeholderRect.left += caretWidth + 3 * dpi_ / 96;
            DrawTextW(dc, L"说点什么…", -1, &placeholderRect,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SetTextColor(dc, placeholderColor);
            SetBkMode(dc, placeholderMode);
            if (placeholderFont) {
                SelectObject(dc, placeholderFont);
            }
            return;
        }

        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        const int oldMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, theme.text);

        const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
        if (!text.empty()) {
            DrawTextW(dc, text.c_str(), -1, &textRect, flags);
        }

        SIZE head = {};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &head);

        if (!composing.empty()) {
            // 组字串接在已上屏文字后面，单独用亮色 + 一条细线标出「还没确认」
            RECT composingRect = textRect;
            composingRect.left += head.cx;
            SetTextColor(dc, theme.accentHi);
            DrawTextW(dc, composing.c_str(), -1, &composingRect, flags);

            SIZE composingSize = {};
            GetTextExtentPoint32W(dc, composing.c_str(), static_cast<int>(composing.size()), &composingSize);
            const int underlineY = static_cast<int>(textRect.top)
                + static_cast<int>(textRect.bottom - textRect.top) / 2 + head.cy / 2 + 1;
            RECT underline = { composingRect.left, underlineY,
                               composingRect.left + composingSize.cx, underlineY + 1 };
            HBRUSH brush = CreateSolidBrush(theme.accent);
            FillRect(dc, &underline, brush);
            DeleteObject(brush);
            head.cx += composingSize.cx;
        }

        SetTextColor(dc, oldColor);
        SetBkMode(dc, oldMode);
        if (oldFont) {
            SelectObject(dc, oldFont);
        }

        DrawCaret(dc, theme, textRect, head.cx);
    }

    // 自己画光标：EDIT 被缩到 1x1 之后它的系统光标没有意义了。
    // 闪烁用时间片算，不额外起定时器（绘制本来就被 150ms 的刷新节拍带着走）。
    void QuickInputBar::DrawCaret(HDC dc, const UiTheme& theme, const RECT& textRect, int offsetX) {
        if ((GetTickCount64() / 500) % 2 != 0) {
            return;
        }
        const int width = (std::max)(1, kUnderlineThicknessLogical * dpi_ / 96 / 2);
        // RECT 成员是 LONG，与 int 不同型，std::max 会推导失败，先取成 int
        const int boxHeight = static_cast<int>(textRect.bottom - textRect.top);
        const int height = (std::max)(8, boxHeight * 3 / 4);
        const int centerY = (static_cast<int>(textRect.top) + static_cast<int>(textRect.bottom)) / 2;
        RECT caret = { textRect.left + offsetX, centerY - height / 2,
                       textRect.left + offsetX + width, centerY - height / 2 + height };
        HBRUSH brush = CreateSolidBrush(theme.text);
        FillRect(dc, &caret, brush);
        DeleteObject(brush);
    }

    HFONT QuickInputBar::EnsureFont(const UiTheme& theme) {
        // 换主题要换字形（雅黑 <-> 楷体斜体），所以字体跟着主题参数一起判定。
        if (font_ && fontFamily_ == theme.fontSans && fontItalic_ == theme.italic) {
            return font_;
        }
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        font_ = CreateFontW(
            -MulDiv(12, dpi_, 72), 0, 0, 0, FW_NORMAL, theme.italic ? TRUE : FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, theme.fontSans);
        fontFamily_ = theme.fontSans;
        fontItalic_ = theme.italic;
        return font_;
    }

    void QuickInputBar::ApplyLayeredStyle() {
        if (!hwnd_) {
            return;
        }

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if (style_ == Style::Underline) {
            // 色键透明：整窗铺键色（纯黑），除了白字与下划线之外全部被系统抠掉，
            // 于是桌面上只剩「一条线 + 浮在上面的字」。
            // 用纯黑当键色是安全的：可见内容里只有主题色与正文色，都不是纯黑。
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd_, kTransparentKey, 0, LWA_COLORKEY);
        }
        else {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        }
        // 扩展样式改动要过一次 SetWindowPos 才会真正生效
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void QuickInputBar::Show(HWND anchor, std::function<void(const std::string&)> onSubmit, Style style) {
        if (!EnsureWindow()) {
            return;
        }

        style_ = style;
        ApplyLayeredStyle();

        // 重复调用视为「重新开一次」：清掉上次残留的文本与回调，避免旧回调被再次触发。
        onSubmit_ = std::move(onSubmit);
        if (edit_) {
            SetWindowTextW(edit_, L"");
        }

        dpi_ = DpiForWindow(anchor ? anchor : hwnd_);
        const UiTheme theme = CurrentUiTheme();
        const int width = kBarWidthLogical * dpi_ / 96;
        const int height = (style_ == Style::Underline ? kUnderlineHeightLogical : kBarHeightLogical) * dpi_ / 96;

        int x = 0;
        int y = 0;
        ComputePosition(anchor, width, height, x, y);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);

        if (edit_) {
            const int pad = kEditPadLogical * dpi_ / 96;
            if (style_ == Style::Underline) {
                // EDIT 只当「键盘与输入法的接收器」用，不参与显示：
                // 它的窗口表面会把父窗口画在它下面的文字整块盖掉（分层窗口里子控件
                // 虽然画不出来，但依然挡光），所以缩成 1x1 塞进下划线那一条里，
                // 把显示区完全让给父窗口自己画。
                SetWindowPos(edit_, nullptr, pad, (std::max)(0, height - 2), 1, 1, SWP_NOZORDER);
            }
            else {
                SetWindowPos(edit_, nullptr, pad, pad, width - pad * 2, height - pad * 2, SWP_NOZORDER);
            }

            HFONT font = EnsureFont(theme);
            if (font) {
                SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            SetFocus(edit_);
            // 光标定位到文本末尾（此时文本为空）
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        }

        visible_ = true;
        // 先提到前台再抢焦点：桌宠窗口是 TOOLWINDOW，不主动激活时输入条会拿不到键盘。
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        if (style_ == Style::Underline) {
            // 父窗口代画 EDIT 的内容，所以需要一个周期刷新来跟上输入法组字。
            // 只在可见期间跑，150ms 对这个尺寸的窗口可以忽略。
            SetTimer(hwnd_, IDT_REFRESH, 150, nullptr);
        }
        else {
            KillTimer(hwnd_, IDT_REFRESH);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
        LOG_DEBUG("QuickInputBar", std::string(style_ == Style::Underline ? "Underline" : "Popup") +
            " at (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    }

    void QuickInputBar::Close() {
        // 回调与文本都在这里结算：隐藏之后再有按键进来不应该还能提交。
        onSubmit_ = nullptr;
        if (edit_) {
            SetWindowTextW(edit_, L"");
        }
        if (hwnd_) {
            KillTimer(hwnd_, IDT_REFRESH);
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

            const UiTheme theme = CurrentUiTheme();

            if (style_ == Style::Underline) {
                // 整窗先铺键色（= 全透明），只在下沿画一条线。
                FillRect(dc, &rc, EnsureBrush(keyBrush_, keyBrushColor_, kTransparentKey));

                const int inset = kUnderlineInsetLogical * dpi_ / 96;
                const int thickness = (std::max)(1, kUnderlineThicknessLogical * dpi_ / 96);
                RECT line = { rc.left + inset, rc.bottom - inset - thickness, rc.right - inset, rc.bottom - inset };
                HBRUSH lineBrush = CreateSolidBrush(theme.accent);
                FillRect(dc, &line, lineBrush);
                DeleteObject(lineBrush);

                // 文字必须由父窗口自己画：分层窗口只合成「窗口自己画出来的」像素，
                // 子控件 EDIT 的绘制会被整个丢掉（实测：EDIT 自己的输入法光标在
                // ——那是系统光标——但字一个都不见）。输入、输入法、选区仍然交给
                // 原生 EDIT，这里只负责把它的内容按同一套字体/位置贴出来。
                DrawEditContent(dc, theme);
            }
            else {
                FillRect(dc, &rc, EnsureBrush(backgroundBrush_, backgroundBrushColor_, theme.surfaceDeep));

                // 描边：与配置终端的冷冰蓝细边一致（？？？时换成暗红）
                HPEN pen = CreatePen(PS_SOLID, 1, theme.edge);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        // EDIT 的文字变了：下划线式是父窗口代画的，必须自己重画一次
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_EDIT && HIWORD(wParam) == EN_CHANGE && style_ == Style::Underline) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TIMER: {
            // 下划线式的周期重画：输入法组字期间 EDIT 不会发 EN_CHANGE，
            // 靠这个把组字串刷出来；顺带让光标位置保持最新。
            if (wParam == IDT_REFRESH && style_ == Style::Underline) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }

        // 输入框的底色与文字色：原生 EDIT 默认是白底黑字，与深色输入条格格不入。
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const UiTheme theme = CurrentUiTheme();
            SetTextColor(dc, theme.text);

            if (style_ == Style::Underline) {
                // 底色必须是键色，否则编辑区会留一块不透明的黑底
                SetBkColor(dc, kTransparentKey);
                return reinterpret_cast<LRESULT>(EnsureBrush(keyBrush_, keyBrushColor_, kTransparentKey));
            }

            SetBkColor(dc, theme.surfaceDeep);
            return reinterpret_cast<LRESULT>(EnsureBrush(backgroundBrush_, backgroundBrushColor_, theme.surfaceDeep));
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
