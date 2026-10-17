#include "../include/SpeechBubble.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"

#include "../include/Logger.h"
#include "../include/UiTheme.h"

#include <algorithm>
#include <cstdint>

namespace UnnamedBot {

    const wchar_t* SpeechBubble::WINDOW_CLASS_NAME = L"UnnamedBotSpeechBubble";

    namespace {

        // 逻辑像素（96 DPI）
        constexpr int kPadLogical = 12;          // 文本内边距
        constexpr int kGapLogical = 10;          // 与锚点的间距
        constexpr int kEdgeLogical = 8;          // 与工作区边缘的最小距离
        constexpr int kCursorOffsetLogical = 18; // 跟随时与鼠标指针的错开距离
        constexpr int kMaxWidthLogical = 300;    // 文本换行宽度上限
        constexpr int kRadiusLogical = 8;        // 圆角半径

        // 底色/描边/文字都来自 UiTheme（？？？时整体换血红），这里只留透明度。
        const BYTE kBubbleAlpha = 236;

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

        // 停留时长：基础 4 秒 + 每字 40ms，夹在 [4s, 12s]。
        // 短句（「？？？」模式基本都是短句）快速掠过，长回复才有时间读完。
        UINT AutoHideMilliseconds(size_t length) {
            const size_t base = 4000;
            const size_t extra = length * 40;
            const size_t total = std::min<size_t>(base + extra, 12000);
            return static_cast<UINT>(std::max<size_t>(total, base));
        }

        // FNV-1a：把文本映射成确定性偏移。
        // 位置不是随机取点：同一段文本总是落在同一处，不同文本之间又有区分度，
        // 因此气泡始终贴着参照物，又不会在连续回复时叠成一张。
        unsigned HashText(const std::wstring& text) {
            unsigned hash = 2166136261u;
            for (wchar_t ch : text) {
                hash ^= static_cast<unsigned>(ch);
                hash *= 16777619u;
            }
            return hash;
        }

    } // namespace

    SpeechBubble& SpeechBubble::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（与其它单例一致）。
        static SpeechBubble* instance = new SpeechBubble();
        return *instance;
    }

    bool SpeechBubble::Initialize(HINSTANCE hInstance) {
        instance_ = hInstance;
        return EnsureWindow();
    }

    void SpeechBubble::Shutdown() {
        if (hwnd_) {
            KillTimer(hwnd_, IDT_AUTOHIDE);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        ReleaseRenderTarget();
        visible_ = false;
    }

    bool SpeechBubble::EnsureWindow() {
        if (hwnd_) {
            return true;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;   // 分层窗口不支持 CS_DROPSHADOW
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_ ? instance_ : GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;

        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(WNDCLASSEXW);
        if (!GetClassInfoExW(wc.hInstance, WINDOW_CLASS_NAME, &existing)) {
            if (!RegisterClassExW(&wc)) {
                LOG_ERROR("SpeechBubble", "Failed to register window class. Error: " + std::to_string(GetLastError()));
                return false;
            }
        }

        // WS_EX_LAYERED 是逐像素透明的前提（UpdateLayeredWindow 配合预乘 alpha）；
        // WS_EX_NOACTIVATE + WS_EX_TRANSPARENT 让它既不抢焦点也不挡鼠标——
        // 气泡只是「贴一句话」，不该干扰用户正在做的事。
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            WINDOW_CLASS_NAME,
            L"",
            WS_POPUP,
            0, 0, 100, 100,
            nullptr, nullptr, wc.hInstance, this
        );
        if (!hwnd_) {
            LOG_ERROR("SpeechBubble", "Failed to create window. Error: " + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    void SpeechBubble::MeasureText(const std::wstring& text, int& width, int& height) const {
        const int pad = kPadLogical * dpi_ / 96;
        const int maxWidth = kMaxWidthLogical * dpi_ / 96;
        const UiTheme theme = CurrentUiTheme();

        HDC dc = GetDC(nullptr);
        // 度量用的字体句柄是临时的：Render 时才真正建常驻字体。
        // 这里必须用与 Render 同一套主题字体，否则换行位置与实绘会对不上。
        HFONT font = CreateFontW(
            -MulDiv(12, dpi_, 72), 0, 0, 0, FW_NORMAL, theme.italic ? TRUE : FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, theme.fontSans);
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

        RECT rc = { 0, 0, maxWidth, 0 };
        DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

        if (oldFont) SelectObject(dc, oldFont);
        if (font) DeleteObject(font);
        ReleaseDC(nullptr, dc);

        width = (rc.right - rc.left) + pad * 2;
        height = (rc.bottom - rc.top) + pad * 2;
        // 空文本或纯空白不应产出零尺寸窗口
        // 加括号调用 (std::max)：Windows.h 的 max 宏会破坏 std::max（本项目未定义 NOMINMAX）。
        width = (std::max)(width, pad * 2 + 24 * dpi_ / 96);
        height = (std::max)(height, pad * 2 + 20 * dpi_ / 96);
    }

    void SpeechBubble::ComputePosition(HWND anchor, int width, int height, int& x, int& y) const {
        const int gap = kGapLogical * dpi_ / 96;
        const int edge = kEdgeLogical * dpi_ / 96;

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

        // 参照物取锚点窗口的中心，而不是窗口矩形：桌宠窗口大部分是透明的，
        // 可见内容（模型 / 圆点）就在中心，贴边算出来的距离会远得莫名其妙。
        int centerX = work.left + (work.right - work.left) / 2;
        int centerY = work.top + (work.bottom - work.top) / 2;
        if (anchor && IsWindow(anchor)) {
            RECT anchorRect = {};
            if (GetWindowRect(anchor, &anchorRect)) {
                centerX = anchorRect.left + (anchorRect.right - anchorRect.left) / 2;
                centerY = anchorRect.top + (anchorRect.bottom - anchorRect.top) / 2;
            }
        }

        // 优先右侧 → 左侧 → 上方 → 下方，取第一个能完整放进工作区的方向。
        const int rightX = centerX + gap;
        const int leftX = centerX - gap - width;
        const int upY = centerY - gap - height;
        const int downY = centerY + gap;

        if (rightX + width <= work.right - edge) {
            x = rightX;
            y = centerY - height / 2;
        }
        else if (leftX >= work.left + edge) {
            x = leftX;
            y = centerY - height / 2;
        }
        else if (upY >= work.top + edge) {
            x = centerX - width / 2;
            y = upY;
        }
        else if (downY + height <= work.bottom - edge) {
            x = centerX - width / 2;
            y = downY;
        }
        else {
            // 四条边都放不下（锚点贴着角落）：退回右上角，至少不会压住半个屏幕
            x = work.right - width - edge;
            y = work.top + edge;
        }

        // 文本决定的确定性错位：范围 ±12 逻辑像素，不足以离开锚点附近，
        // 但足以让连续两条回复不完全重合。
        const unsigned hash = HashText(text_);
        const int jitter = 12 * dpi_ / 96;
        x += static_cast<int>(hash % static_cast<unsigned>(jitter * 2 + 1)) - jitter;
        y += static_cast<int>((hash >> 8) % static_cast<unsigned>(jitter * 2 + 1)) - jitter;

        x = std::clamp(x, workLeft + edge, (std::max)(workLeft + edge, workRight - width - edge));
        y = std::clamp(y, workTop + edge, (std::max)(workTop + edge, workBottom - height - edge));
    }

    void SpeechBubble::Show(HWND anchor, const std::wstring& text) {
        ShowInternal(anchor, text, 0);
    }

    void SpeechBubble::ShowAtCursor(const std::wstring& text) {
        // 指针位置取不到（极少见）时 ShowInternal 会退回锚点窗口的算法
        ShowInternal(nullptr, text, 1);
    }

    void SpeechBubble::ShowNearActiveWindow(const std::wstring& text) {
        ShowInternal(nullptr, text, 2);
    }

    void SpeechBubble::ShowAnchored(const std::wstring& text) {
        // 贴哪儿由设置决定：吸附鼠标（1）还是吸附活动窗口（2）
        const std::string anchor = Utils::ToLower(
            ConfigManager::Instance().GetString(ConfigManager::KEY_UI_REPLY_ANCHOR, "window"));
        ShowInternal(nullptr, text, anchor == "cursor" ? 1 : 2);
    }

    // 两种落点共用的显示流程：量文本 → 定位 → 建渲染目标 → 画 → 显示 → 重新计时
    void SpeechBubble::ShowInternal(HWND anchor, const std::wstring& text, int placement) {
        if (text.empty()) {
            Hide();
            return;
        }
        if (!EnsureWindow()) {
            return;
        }

        text_ = text;
        anchor_ = anchor;
        dpi_ = DpiForWindow(anchor ? anchor : hwnd_);

        int width = 0;
        int height = 0;
        MeasureText(text_, width, height);

        switch (placement) {
        case 1:  ComputePositionAtCursor(width, height, bubbleX_, bubbleY_); break;
        case 2:  ComputePositionNearActiveWindow(width, height, bubbleX_, bubbleY_); break;
        default: ComputePosition(anchor, width, height, bubbleX_, bubbleY_); break;
        }

        if (!EnsureRenderTarget(width, height)) {
            return;
        }
        Render();

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

        // 重新计时：连续两条回复应该是「新的把旧的顶掉」，而不是排队消失。
        KillTimer(hwnd_, IDT_AUTOHIDE);
        SetTimer(hwnd_, IDT_AUTOHIDE, AutoHideMilliseconds(text_.size()), nullptr);
        visible_ = true;
    }

    // 跟着鼠标指针落点：贴在指针右下方（错开一点，别压住指针本身），
    // 右边/下面放不下就翻到左边/上面，最后整体夹进当前显示器的工作区。
    // 按当前活动窗口定位：回复要落在「你正在用的那个窗口」里。
    // 多窗口 / 多显示器时，纯屏幕坐标会让气泡飘到无关的地方去。
    void SpeechBubble::ComputePositionNearActiveWindow(int width, int height, int& x, int& y) const {
        HWND foreground = GetForegroundWindow();

        // 前台窗口是我们自己的（桌宠 / 配置中枢 / 气泡 / 输入条）时，
        // 说明用户正在跟桌宠打交道 —— 回到「贴桌宠」的老行为最自然。
        bool ours = false;
        if (foreground) {
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            ours = (pid == GetCurrentProcessId()) || foreground == hwnd_ || foreground == anchor_;
        }
        if (!foreground || ours) {
            ComputePosition(anchor_, width, height, x, y);
            return;
        }

        RECT target = {};
        if (!GetWindowRect(foreground, &target) ||
            (target.right - target.left) < 80 || (target.bottom - target.top) < 60) {
            // 拿不到像样的窗口矩形（比如前台是桌面/任务栏）→ 退回指针算法
            ComputePositionAtCursor(width, height, x, y);
            return;
        }

        const int offset = kCursorOffsetLogical * dpi_ / 96;
        const int edge = kEdgeLogical * dpi_ / 96;

        POINT cursor = {};
        const bool hasCursor = GetCursorPos(&cursor) != FALSE;
        const bool cursorInside = hasCursor &&
            cursor.x >= target.left && cursor.x <= target.right &&
            cursor.y >= target.top && cursor.y <= target.bottom;

        int px = 0;
        int py = 0;
        if (cursorInside) {
            // 指针就在这个窗口里：贴着指针，但整块气泡必须留在窗口内
            px = cursor.x + offset;
            py = cursor.y + offset;
            if (px + width > target.right - edge) px = cursor.x - offset - width;
            if (py + height > target.bottom - edge) py = cursor.y - offset - height;
        }
        else {
            // 指针在别处：贴这个窗口的右下角
            px = target.right - width - offset;
            py = target.bottom - height - offset;
        }

        // 屏幕工作区还是要夹一次：窗口可能贴着屏幕边或被最大化
        MONITORINFO info = {};
        info.cbSize = sizeof(MONITORINFO);
        HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &info)) {
            info.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
        const int workLeft = static_cast<int>(info.rcWork.left);
        const int workTop = static_cast<int>(info.rcWork.top);
        const int workRight = static_cast<int>(info.rcWork.right);
        const int workBottom = static_cast<int>(info.rcWork.bottom);

        // 先夹进目标窗口，再夹进工作区（窗口本身一般就在工作区内）
        const int minX = (std::max)(static_cast<int>(target.left) + edge, workLeft + edge);
        const int minY = (std::max)(static_cast<int>(target.top) + edge, workTop + edge);
        const int maxX = (std::min)(static_cast<int>(target.right) - width - edge,
                                    (std::max)(minX, workRight - width - edge));
        const int maxY = (std::min)(static_cast<int>(target.bottom) - height - edge,
                                    (std::max)(minY, workBottom - height - edge));

        x = std::clamp(px, minX, (std::max)(minX, maxX));
        y = std::clamp(py, minY, (std::max)(minY, maxY));

        // 与其它落点方式一致的、由文本决定的确定性错位：连续两条不会完全重合
        const unsigned hash = HashText(text_);
        const int jitter = 8 * dpi_ / 96;
        x = std::clamp(x + static_cast<int>(hash % static_cast<unsigned>(jitter * 2 + 1)) - jitter,
            minX, (std::max)(minX, maxX));
        y = std::clamp(y + static_cast<int>((hash >> 8) % static_cast<unsigned>(jitter * 2 + 1)) - jitter,
            minY, (std::max)(minY, maxY));
    }

    void SpeechBubble::ComputePositionAtCursor(int width, int height, int& x, int& y) const {
        POINT cursor = {};
        if (!GetCursorPos(&cursor)) {
            // 取不到指针：退回「锚到桌宠」的老路，至少还有个合理位置
            ComputePosition(anchor_, width, height, x, y);
            return;
        }

        const int offset = kCursorOffsetLogical * dpi_ / 96;
        const int edge = kEdgeLogical * dpi_ / 96;

        MONITORINFO info = {};
        info.cbSize = sizeof(MONITORINFO);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &info)) {
            info.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
        const RECT work = info.rcWork;
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);

        // 右下 → 左下 → 右上 → 左上
        int px = cursor.x + offset;
        int py = cursor.y + offset;
        if (px + width > workRight - edge) {
            px = cursor.x - offset - width;
        }
        if (py + height > workBottom - edge) {
            py = cursor.y - offset - height;
        }

        x = std::clamp(px, workLeft + edge, (std::max)(workLeft + edge, workRight - width - edge));
        y = std::clamp(py, workTop + edge, (std::max)(workTop + edge, workBottom - height - edge));

        // 与锚点窗口那套一样，叠一点由文本决定的确定性错位：连续两条不会完全重合
        const unsigned hash = HashText(text_);
        const int jitter = 10 * dpi_ / 96;
        x += static_cast<int>(hash % static_cast<unsigned>(jitter * 2 + 1)) - jitter;
        y = std::clamp(y + static_cast<int>((hash >> 8) % static_cast<unsigned>(jitter * 2 + 1)) - jitter,
            workTop + edge, (std::max)(workTop + edge, workBottom - height - edge));
        x = std::clamp(x, workLeft + edge, (std::max)(workLeft + edge, workRight - width - edge));
    }

    void SpeechBubble::Hide() {
        if (hwnd_) {
            KillTimer(hwnd_, IDT_AUTOHIDE);
            ShowWindow(hwnd_, SW_HIDE);
        }
        visible_ = false;
    }

    bool SpeechBubble::IsVisible() const {
        return hwnd_ && IsWindowVisible(hwnd_) != FALSE;
    }

    void SpeechBubble::OnAutoHideTimer() {
        Hide();
    }

    void SpeechBubble::ReleaseRenderTarget() {
        if (memDc_) {
            if (oldBitmap_) {
                SelectObject(memDc_, oldBitmap_);
                oldBitmap_ = nullptr;
            }
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        bitmapBits_ = nullptr;
        bitmapWidth_ = 0;
        bitmapHeight_ = 0;
    }

    bool SpeechBubble::EnsureRenderTarget(int width, int height) {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (memDc_ && bitmap_ && bitmapWidth_ == width && bitmapHeight_ == height) {
            return true;
        }

        ReleaseRenderTarget();

        HDC screenDc = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screenDc);

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        // 负高度：自上而下的 DIB，行序与 UpdateLayeredWindow 期望的一致
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        bitmap_ = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDc);

        if (!memDc_ || !bitmap_ || !bits) {
            LOG_ERROR("SpeechBubble", "Failed to create layered window render target.");
            ReleaseRenderTarget();
            return false;
        }

        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, bitmap_));
        bitmapBits_ = bits;
        bitmapWidth_ = width;
        bitmapHeight_ = height;
        return true;
    }

    void SpeechBubble::Render() {
        if (!hwnd_ || !memDc_ || !bitmapBits_ || bitmapWidth_ <= 0 || bitmapHeight_ <= 0) {
            return;
        }

        const int width = bitmapWidth_;
        const int height = bitmapHeight_;
        const int pad = kPadLogical * dpi_ / 96;
        const int radius = kRadiusLogical * dpi_ / 96;

        // GDI 不写 alpha 字节，所以分三步走：
        // 1) 整块缓冲清零（同时把 alpha 清成 0）；
        // 2) 只用 GDI 画形状与文字（它只写 RGB）；
        // 3) 按像素判定「属于哪一层」，补 alpha 并预乘。
        memset(bitmapBits_, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

        const UiTheme theme = CurrentUiTheme();
        const COLORREF bubbleColor = theme.surface;
        const COLORREF borderColor = theme.edge;
        const COLORREF textColor = theme.text;

        HBRUSH fill = CreateSolidBrush(bubbleColor);
        HPEN border = CreatePen(PS_SOLID, 1, borderColor);
        HGDIOBJ oldBrush = SelectObject(memDc_, fill);
        HGDIOBJ oldPen = SelectObject(memDc_, border);
        RoundRect(memDc_, 0, 0, width, height, radius * 2, radius * 2);
        SelectObject(memDc_, oldPen);
        SelectObject(memDc_, oldBrush);
        DeleteObject(fill);
        DeleteObject(border);

        if (!font_ || fontFamily_ != theme.fontSans || fontItalic_ != theme.italic) {
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
        }
        HGDIOBJ oldFont = font_ ? SelectObject(memDc_, font_) : nullptr;
        SetBkMode(memDc_, TRANSPARENT);
        SetTextColor(memDc_, textColor);
        RECT textRect = { pad, pad, width - pad, height - pad };
        DrawTextW(memDc_, text_.c_str(), -1, &textRect, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        if (oldFont) {
            SelectObject(memDc_, oldFont);
        }

        const unsigned char bgB = GetBValue(bubbleColor);
        const unsigned char bgG = GetGValue(bubbleColor);
        const unsigned char bgR = GetRValue(bubbleColor);

        unsigned char* pixels = static_cast<unsigned char*>(bitmapBits_);
        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < count; ++i) {
            unsigned char* pixel = pixels + i * 4;   // BGRA
            const unsigned char blue = pixel[0];
            const unsigned char green = pixel[1];
            const unsigned char red = pixel[2];

            unsigned char alpha = 0;
            if (blue == 0 && green == 0 && red == 0) {
                alpha = 0;                       // 没画到：圆角之外，保持透明
            }
            else if (blue == bgB && green == bgG && red == bgR) {
                alpha = kBubbleAlpha;            // 气泡底色：略带透明，不闷住桌面
            }
            else {
                alpha = 255;                     // 边框与文字：实心
            }

            // 预乘：UpdateLayeredWindow 要求 AC_SRC_ALPHA 的缓冲是预乘过的
            pixel[0] = static_cast<unsigned char>(blue * alpha / 255);
            pixel[1] = static_cast<unsigned char>(green * alpha / 255);
            pixel[2] = static_cast<unsigned char>(red * alpha / 255);
            pixel[3] = alpha;
        }

        HDC screenDc = GetDC(nullptr);
        POINT destination = { bubbleX_, bubbleY_ };
        SIZE size = { width, height };
        POINT source = { 0, 0 };
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        if (!UpdateLayeredWindow(hwnd_, screenDc, &destination, &size, memDc_, &source, 0, &blend, ULW_ALPHA)) {
            LOG_WARN("SpeechBubble", "UpdateLayeredWindow failed. Error: " + std::to_string(GetLastError()));
        }
        ReleaseDC(nullptr, screenDc);
    }

    LRESULT CALLBACK SpeechBubble::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        SpeechBubble* bubble = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            bubble = reinterpret_cast<SpeechBubble*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bubble));
        }
        else {
            bubble = reinterpret_cast<SpeechBubble*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (bubble) {
            return bubble->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT SpeechBubble::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_TIMER: {
            if (wParam == IDT_AUTOHIDE) {
                OnAutoHideTimer();
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND:
            // 分层窗口的内容全由 UpdateLayeredWindow 提交，系统擦除只会闪一下
            return 1;

        case WM_NCHITTEST:
            // 与 WS_EX_TRANSPARENT 双重保险：气泡永不接管鼠标
            return HTTRANSPARENT;

        case WM_DESTROY: {
            KillTimer(hwnd, IDT_AUTOHIDE);
            hwnd_ = nullptr;
            visible_ = false;
            ReleaseRenderTarget();
            return 0;
        }

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace UnnamedBot
