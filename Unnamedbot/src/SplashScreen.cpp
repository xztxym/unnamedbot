#include "../include/SplashScreen.h"

#include <cwchar>

namespace UnnamedBot {

    const wchar_t* SplashScreen::WINDOW_CLASS_NAME = L"UnnamedBotSplashScreen";

    namespace {

        // ===== 与控制终端一致的配色（prototype (4).html）=====
        const COLORREF kBgTop = RGB(0x0a, 0x15, 0x30);
        const COLORREF kBgBottom = RGB(0x06, 0x0b, 0x19);
        const COLORREF kCardEdge = RGB(0x2b, 0x3c, 0x5f);
        const COLORREF kInputBg = RGB(0x07, 0x0d, 0x1e);
        const COLORREF kHairline = RGB(0x17, 0x21, 0x3b);
        const COLORREF kTextSecondary = RGB(0xa9, 0xbd, 0xe0);
        const COLORREF kTextMuted = RGB(0x73, 0x8b, 0xb6);
        const COLORREF kIceHigh = RGB(0xe0, 0xed, 0xff);
        const COLORREF kIceMid = RGB(0x86, 0xae, 0xf2);
        const COLORREF kAccent = RGB(0x87, 0xb2, 0xff);

        // ===== 逻辑尺寸（96 DPI）=====
        const int kWinW = 470;
        const int kWinH = 258;
        const int kMargin = 30;

        int ClampInt(int value, int low, int high) {
            return value < low ? low : (value > high ? high : value);
        }

        void FillColor(HDC hdc, const RECT& rc, COLORREF color) {
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);
        }

        // 垂直渐变：加载界面只有一层底色，逐行填充即可
        void FillVerticalGradient(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom) {
            const int height = rc.bottom - rc.top;
            if (height <= 0) return;
            const int r0 = GetRValue(top), g0 = GetGValue(top), b0 = GetBValue(top);
            const int r1 = GetRValue(bottom), g1 = GetGValue(bottom), b1 = GetBValue(bottom);
            for (int i = 0; i < height; ++i) {
                const float t = (height > 1) ? static_cast<float>(i) / static_cast<float>(height - 1) : 0.0f;
                const COLORREF color = RGB(
                    static_cast<int>(r0 + (r1 - r0) * t),
                    static_cast<int>(g0 + (g1 - g0) * t),
                    static_cast<int>(b0 + (b1 - b0) * t));
                const RECT line = { rc.left, rc.top + i, rc.right, rc.top + i + 1 };
                FillColor(hdc, line, color);
            }
        }

        // 水平渐变：进度条已完成部分
        void FillHorizontalGradient(HDC hdc, const RECT& rc, COLORREF left, COLORREF right) {
            const int width = rc.right - rc.left;
            if (width <= 0) return;
            const int r0 = GetRValue(left), g0 = GetGValue(left), b0 = GetBValue(left);
            const int r1 = GetRValue(right), g1 = GetGValue(right), b1 = GetBValue(right);
            for (int i = 0; i < width; ++i) {
                const float t = (width > 1) ? static_cast<float>(i) / static_cast<float>(width - 1) : 0.0f;
                const COLORREF color = RGB(
                    static_cast<int>(r0 + (r1 - r0) * t),
                    static_cast<int>(g0 + (g1 - g0) * t),
                    static_cast<int>(b0 + (b1 - b0) * t));
                const RECT line = { rc.left + i, rc.top, rc.left + i + 1, rc.bottom };
                FillColor(hdc, line, color);
            }
        }

        void DrawBoxBorder(HDC hdc, const RECT& rc, COLORREF color) {
            const RECT top = { rc.left, rc.top, rc.right, rc.top + 1 };
            const RECT bottom = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
            const RECT left = { rc.left, rc.top, rc.left + 1, rc.bottom };
            const RECT right = { rc.right - 1, rc.top, rc.right, rc.bottom };
            FillColor(hdc, top, color);
            FillColor(hdc, bottom, color);
            FillColor(hdc, left, color);
            FillColor(hdc, right, color);
        }

        // 原型里大量使用 letter-spacing，GDI 需要逐字绘制来复现
        int SpacedTextWidth(HDC hdc, const std::wstring& text, int extra) {
            if (text.empty()) return 0;
            SIZE size = {};
            int width = 0;
            for (wchar_t ch : text) {
                GetTextExtentPoint32W(hdc, &ch, 1, &size);
                width += size.cx + extra;
            }
            return width - extra;
        }

        void DrawSpacedText(HDC hdc, const std::wstring& text, int x, int y, int extra, COLORREF color) {
            SetTextColor(hdc, color);
            int cursor = x;
            SIZE size = {};
            for (wchar_t ch : text) {
                TextOutW(hdc, cursor, y, &ch, 1);
                GetTextExtentPoint32W(hdc, &ch, 1, &size);
                cursor += size.cx + extra;
            }
        }

        // GetDpiForWindow 需要 Windows 10 1607+，运行时解析以免抬高最低系统要求
        int DpiForWindowSafe(HWND hwnd) {
            typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
            static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            if (fn != nullptr && hwnd != nullptr) {
                const UINT dpi = fn(hwnd);
                if (dpi != 0) return static_cast<int>(dpi);
            }
            HDC screen = GetDC(nullptr);
            const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
            if (screen) ReleaseDC(nullptr, screen);
            return dpi > 0 ? dpi : 96;
        }

    } // namespace

    SplashScreen& SplashScreen::Instance() {
        // 与 ControlTerminalWindow 一致：不随静态存储期销毁，进程退出阶段仍可能被调用。
        static SplashScreen* instance = new SplashScreen();
        return *instance;
    }

    // ============================================================
    // 生命周期
    // ============================================================

    bool SplashScreen::Create(HINSTANCE hInstance) {
        if (hInstance == nullptr) {
            return false;
        }
        if (thread_.joinable()) {
            // 已经在运行：重复调用只当成功处理
            return hwnd_.load() != nullptr;
        }

        instance_ = hInstance;
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_ = false;
            created_ = false;
        }

        thread_ = std::thread(&SplashScreen::ThreadMain, this, hInstance);

        // 等窗口真正可见再返回，避免调用方紧接着的重活把建窗消息压后。
        // 窗口类注册失败等情况下工作线程会立刻置位 ready_，不阻塞启动。
        {
            std::unique_lock<std::mutex> lock(readyMutex_);
            readyCv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_; });
            return created_;
        }
    }

    void SplashScreen::Shutdown() {
        const HWND hwnd = hwnd_.load();
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_REQUEST_QUIT, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        hwnd_ = nullptr;
    }

    void SplashScreen::ThreadMain(HINSTANCE hInstance) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &SplashScreen::WindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // 全部由 WM_PAINT 绘制，避免闪烁
        wc.lpszClassName = WINDOW_CLASS_NAME;

        if (RegisterClassExW(&wc) == 0) {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_ = true;
            readyCv_.notify_all();
            return;
        }

        // 先按 96 DPI 建窗，WM_CREATE 里拿到真实 DPI 后再调整尺寸
        HWND hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            WINDOW_CLASS_NAME, L"UnnamedBot",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, kWinW, kWinH,
            nullptr, nullptr, hInstance, this);
        if (hwnd == nullptr) {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_ = true;
            readyCv_.notify_all();
            return;
        }

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);

        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            created_ = true;
            ready_ = true;
        }
        readyCv_.notify_all();

        MSG msg = {};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ============================================================
    // 状态更新（初始化线程调用）
    // ============================================================

    void SplashScreen::SetStatus(const std::wstring& text, float progress) {
        // windows.h 定义了 min/max 宏，这里显式截断
        float clamped = progress;
        if (clamped < 0.0f) clamped = 0.0f;
        if (clamped > 1.0f) clamped = 1.0f;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_ = text;
            progress_ = clamped;
        }
        const HWND hwnd = hwnd_.load();
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_STATUS_CHANGED, 0, 0);
        }
    }

    void SplashScreen::SetStatus(const std::wstring& text) {
        float progress = 0.0f;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            progress = progress_;
        }
        SetStatus(text, progress);
    }

    // ============================================================
    // 窗口过程（界面线程）
    // ============================================================

    LRESULT CALLBACK SplashScreen::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        SplashScreen* self = reinterpret_cast<SplashScreen*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<SplashScreen*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT SplashScreen::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            return OnCreate(hwnd);

        case WM_ERASEBKGND:
            return 1;   // 背景由 OnPaint 全量覆盖

        case WM_PAINT:
            OnPaint(hwnd);
            return 0;

        case WM_STATUS_CHANGED:
        case WM_TIMER:
            // WM_TIMER 驱动流动高光与已用时长，主线程阻塞时界面线程照常运行
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCHITTEST:
            // 无标题栏：允许按住任意位置拖动加载界面
            return HTCAPTION;

        case WM_REQUEST_QUIT:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ANIMATION);
            OnDestroy();
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT SplashScreen::OnCreate(HWND hwnd) {
        hwnd_ = hwnd;
        dpi_ = DpiForWindowSafe(hwnd);
        CreateFonts();

        // 居中到工作区（避开任务栏）
        RECT work = {};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            const int width = Scale(kWinW);
            const int height = Scale(kWinH);
            const int x = work.left + ((work.right - work.left) - width) / 2;
            const int y = work.top + ((work.bottom - work.top) - height) / 2;
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        }

        startTick_ = GetTickCount64();
        SetTimer(hwnd, TIMER_ANIMATION, 40, nullptr);
        return 0;
    }

    void SplashScreen::OnDestroy() {
        DestroyFonts();
    }

    void SplashScreen::CreateFonts() {
        const wchar_t* sans = L"Microsoft YaHei UI";
        const wchar_t* mono = L"Cascadia Mono";

        auto make = [this](int pt, int weight, const wchar_t* face) {
            return CreateFontW(-Scale(pt), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, face);
        };

        titleFont_ = make(24, FW_SEMIBOLD, sans);
        bodyFont_ = make(12, FW_NORMAL, sans);
        monoFont_ = make(10, FW_NORMAL, mono);
        monoSmFont_ = make(8, FW_NORMAL, mono);
    }

    void SplashScreen::DestroyFonts() {
        HFONT fonts[] = { titleFont_, bodyFont_, monoFont_, monoSmFont_ };
        for (HFONT font : fonts) {
            if (font) DeleteObject(font);
        }
        titleFont_ = nullptr;
        bodyFont_ = nullptr;
        monoFont_ = nullptr;
        monoSmFont_ = nullptr;
    }

    int SplashScreen::Scale(int value) const {
        return MulDiv(value, dpi_, 96);
    }

    // ============================================================
    // 绘制
    // ============================================================

    void SplashScreen::OnPaint(HWND hwnd) {
        PAINTSTRUCT ps = {};
        HDC target = BeginPaint(hwnd, &ps);
        if (target == nullptr) {
            EndPaint(hwnd, &ps);
            return;
        }

        RECT client = {};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            EndPaint(hwnd, &ps);
            return;
        }

        // 双缓冲：一次渐变填充较贵，直接画到屏幕会闪烁
        HDC memDc = CreateCompatibleDC(target);
        HBITMAP memBitmap = CreateCompatibleBitmap(target, width, height);
        if (memDc == nullptr || memBitmap == nullptr) {
            if (memBitmap) DeleteObject(memBitmap);
            if (memDc) DeleteDC(memDc);
            EndPaint(hwnd, &ps);
            return;
        }
        HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);

        // 状态快照：加锁读取，绘制期间不再访问共享字段
        std::wstring status;
        float progress = 0.0f;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status = status_;
            progress = progress_;
        }

        FillVerticalGradient(memDc, client, kBgTop, kBgBottom);
        DrawBoxBorder(memDc, client, kCardEdge);
        SetBkMode(memDc, TRANSPARENT);

        const int margin = Scale(kMargin);

        // 标题与副标题
        HGDIOBJ oldFont = SelectObject(memDc, titleFont_);
        DrawSpacedText(memDc, L"UnnamedBot", margin, Scale(32), Scale(3), kIceHigh);

        SelectObject(memDc, monoSmFont_);
        DrawSpacedText(memDc, L"DESKTOP PET · BOOT SEQUENCE", margin, Scale(74), Scale(2), kTextMuted);

        const RECT hairline = { margin, Scale(100), width - margin, Scale(100) + 1 };
        FillColor(memDc, hairline, kHairline);

        // 当前阶段
        SelectObject(memDc, bodyFont_);
        SetTextColor(memDc, kTextSecondary);
        RECT statusRect = { margin, Scale(114), width - margin, Scale(140) };
        DrawTextW(memDc, status.c_str(), -1, &statusRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 进度条
        const int trackTop = Scale(156);
        const int trackHeight = Scale(12);
        const RECT track = { margin, trackTop, width - margin, trackTop + trackHeight };
        const int trackWidth = track.right - track.left;
        const int filledWidth = ClampInt(static_cast<int>(trackWidth * progress), 0, trackWidth);

        FillColor(memDc, track, kInputBg);

        // 未完成时，剩余区间有一段高光来回流动：表示「仍在工作」而不是卡死
        if (progress < 1.0f) {
            const int sweepWidth = Scale(48);
            const int remaining = trackWidth - filledWidth;
            if (remaining > sweepWidth) {
                const ULONGLONG period = 1400;
                const ULONGLONG phase = (GetTickCount64() - startTick_) % (period * 2);
                const float t = (phase < period)
                    ? static_cast<float>(phase) / static_cast<float>(period)
                    : 1.0f - static_cast<float>(phase - period) / static_cast<float>(period);
                const int travel = ClampInt(static_cast<int>(t * static_cast<float>(remaining - sweepWidth)),
                    0, remaining - sweepWidth);
                const int sweepLeft = track.left + filledWidth + travel;
                const RECT sweep = { sweepLeft, track.top + Scale(1), sweepLeft + sweepWidth, track.bottom - Scale(1) };
                FillColor(memDc, sweep, kHairline);
            }
        }

        // 已完成部分
        if (filledWidth > 0) {
            const RECT fill = { track.left, track.top + Scale(1), track.left + filledWidth, track.bottom - Scale(1) };
            FillHorizontalGradient(memDc, fill, kIceMid, kAccent);
        }

        DrawBoxBorder(memDc, track, kCardEdge);

        // 页脚：已用时长 + 百分比
        const double elapsed = static_cast<double>(GetTickCount64() - startTick_) / 1000.0;
        wchar_t elapsedText[64] = {};
        swprintf_s(elapsedText, L"ELAPSED %.1fs", elapsed);

        wchar_t percentText[32] = {};
        swprintf_s(percentText, L"%d%%", static_cast<int>(progress * 100.0f + 0.5f));

        SelectObject(memDc, monoSmFont_);
        SetTextColor(memDc, kTextMuted);
        TextOutW(memDc, margin, Scale(184), elapsedText, static_cast<int>(wcslen(elapsedText)));

        const std::wstring percent = percentText;
        const int percentWidth = SpacedTextWidth(memDc, percent, Scale(2));
        DrawSpacedText(memDc, percent, width - margin - percentWidth, Scale(184), Scale(2), kIceHigh);

        // 底部提示：说明这次等待是做什么的
        SelectObject(memDc, monoFont_);
        SetTextColor(memDc, kTextMuted);
        RECT hintRect = { margin, Scale(212), width - margin, Scale(238) };
        DrawTextW(memDc, L"启动时需编译 Live2D 着色器，可能耗时数十秒，请勿关闭窗口。", -1, &hintRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(memDc, oldFont);
        BitBlt(target, 0, 0, width, height, memDc, 0, 0, SRCCOPY);

        SelectObject(memDc, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDc);

        EndPaint(hwnd, &ps);
    }

} // namespace UnnamedBot
