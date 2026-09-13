#include "../include/ControlTerminalWindow.h"
#include "../include/ConfigManager.h"
#include "../include/Logger.h"
#include "../include/Utils.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <cstdint>

namespace UnnamedBot {

    const wchar_t* ControlTerminalWindow::WINDOW_CLASS_NAME = L"UnnamedBotControlTerminal";

    namespace {

        // ===== prototype (4).html 配色（已按层层叠加合成为不透明色）=====
        const COLORREF kBgTop = RGB(0x0a, 0x15, 0x30);        // --bg1
        const COLORREF kBgBottom = RGB(0x06, 0x0b, 0x19);     // --bg0
        const COLORREF kCardTop = RGB(0x0e, 0x16, 0x2e);      // .set-sec 渐变上端
        const COLORREF kCardBottom = RGB(0x09, 0x0f, 0x22);   // .set-sec 渐变下端
        const COLORREF kCardEdge = RGB(0x2b, 0x3c, 0x5f);     // --edge
        const COLORREF kEdgeStrong = RGB(0x5e, 0x7c, 0xa0);   // --edge2
        const COLORREF kHairline = RGB(0x17, 0x21, 0x3b);     // .prow 分隔线
        const COLORREF kInputBg = RGB(0x07, 0x0d, 0x1e);      // .mini-in
        const COLORREF kSegActive = RGB(0x1b, 0x2a, 0x4e);    // 单选选中底
        const COLORREF kGhostFill = RGB(0x13, 0x1d, 0x38);    // .btn 底色
        const COLORREF kPrimaryFill = RGB(0x1f, 0x2d, 0x52);  // .btn.primary / .btn-save
        const COLORREF kTextPrimary = RGB(0xe8, 0xf1, 0xff);  // --txt
        const COLORREF kTextSecondary = RGB(0xa9, 0xbd, 0xe0); // --tx2
        const COLORREF kTextMuted = RGB(0x73, 0x8b, 0xb6);    // --tx3
        const COLORREF kIce = RGB(0xa8, 0xce, 0xff);          // --ice
        const COLORREF kIceHigh = RGB(0xe0, 0xed, 0xff);      // --ice-hi
        const COLORREF kIceMid = RGB(0x86, 0xae, 0xf2);       // --ice-mid
        const COLORREF kAccent = RGB(0x87, 0xb2, 0xff);       // --acc
        const COLORREF kHeading = RGB(0xee, 0xf4, 0xff);
        const COLORREF kButtonText = RGB(0xf2, 0xf7, 0xff);
        const COLORREF kCheckDark = RGB(0x08, 0x10, 0x20);
        const COLORREF kFailText = RGB(0xbc, 0xc8, 0xde);     // --fail-txt
        const COLORREF kFailMark = RGB(0xa6, 0xb2, 0xcc);     // --fail
        const COLORREF kErrorText = RGB(0xcf, 0xd9, 0xea);

        // ===== 逻辑尺寸（96 DPI）=====
        const int kWinW = 900;
        const int kWinH = 720;
        const int kMargin = 28;
        const int kHeaderH = 104;
        const int kFooterH = 84;
        const int kCardPadX = 24;
        const int kCardPadY = 22;
        const int kCardGap = 26;
        const int kRowH = 40;
        const int kLabelW = 176;
        const int kNoteW = 168;
        const int kInputH = 32;
        const int kBtnH = 40;

        int ClampInt(int value, int low, int high) {
            return value < low ? low : (value > high ? high : value);
        }

        // windows.h 定义了 max 宏，所以这里不用 std::max
        int MaxInt(int a, int b) {
            return a > b ? a : b;
        }

        void FillColor(HDC hdc, const RECT& rc, COLORREF color) {
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);
        }

        void FillVerticalGradient(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom) {
            int height = rc.bottom - rc.top;
            if (height <= 0) return;
            int r0 = GetRValue(top), g0 = GetGValue(top), b0 = GetBValue(top);
            int r1 = GetRValue(bottom), g1 = GetGValue(bottom), b1 = GetBValue(bottom);
            for (int i = 0; i < height; ++i) {
                float t = (height > 1) ? static_cast<float>(i) / static_cast<float>(height - 1) : 0.0f;
                COLORREF color = RGB(
                    static_cast<int>(r0 + (r1 - r0) * t),
                    static_cast<int>(g0 + (g1 - g0) * t),
                    static_cast<int>(b0 + (b1 - b0) * t));
                RECT line = { rc.left, rc.top + i, rc.right, rc.top + i + 1 };
                FillColor(hdc, line, color);
            }
        }

        void DrawHRule(HDC hdc, int left, int right, int y, COLORREF color) {
            RECT line = { left, y, right, y + 1 };
            FillColor(hdc, line, color);
        }

        void DrawBoxBorder(HDC hdc, const RECT& rc, COLORREF color) {
            DrawHRule(hdc, rc.left, rc.right, rc.top, color);
            DrawHRule(hdc, rc.left, rc.right, rc.bottom - 1, color);
            RECT left = { rc.left, rc.top, rc.left + 1, rc.bottom };
            FillColor(hdc, left, color);
            RECT right = { rc.right - 1, rc.top, rc.right, rc.bottom };
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

        void DrawSpacedTextRight(HDC hdc, const std::wstring& text, int right, int y, int extra, COLORREF color) {
            DrawSpacedText(hdc, text, right - SpacedTextWidth(hdc, text, extra), y, extra, color);
        }

        // 原型里的菱形强调点（transform: rotate(45deg)）
        void DrawDiamond(HDC hdc, int centerX, int centerY, int radius, COLORREF color) {
            POINT points[4] = {
                { centerX, centerY - radius },
                { centerX + radius, centerY },
                { centerX, centerY + radius },
                { centerX - radius, centerY }
            };
            HBRUSH brush = CreateSolidBrush(color);
            HPEN pen = CreatePen(PS_SOLID, 1, color);
            HGDIOBJ oldBrush = SelectObject(hdc, brush);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            Polygon(hdc, points, 4);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);
            DeleteObject(brush);
        }

        // .btn / .btn-back 的切角矩形（右上、左下各切一角）
        void DrawCutCornerPanel(HDC hdc, const RECT& rc, int cut, COLORREF fill, COLORREF border) {
            POINT points[6] = {
                { rc.left, rc.top },
                { rc.right - cut, rc.top },
                { rc.right, rc.top + cut },
                { rc.right, rc.bottom },
                { rc.left + cut, rc.bottom },
                { rc.left, rc.bottom - cut }
            };
            HBRUSH brush = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oldBrush = SelectObject(hdc, brush);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            Polygon(hdc, points, 6);
            Polyline(hdc, points, 6);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);
            DeleteObject(brush);
        }

        std::vector<std::wstring> SplitOptions(const wchar_t* options) {
            std::vector<std::wstring> result;
            if (!options) return result;
            std::wstring current;
            for (const wchar_t* p = options; *p; ++p) {
                if (*p == L'|') {
                    result.push_back(current);
                    current.clear();
                }
                else {
                    current.push_back(*p);
                }
            }
            result.push_back(current);
            return result;
        }

        // ===== 卡片 01：AI 接口与鉴权 =====
        const ControlTerminalWindow::FieldDef kApiFields[] = {
            { "api.base_url",      L"服务地址", L"API BASE URL", L"OpenAI 兼容 /v1", ControlTerminalWindow::FieldKind::Text, L"https://api.openai.com/v1", nullptr, false, 320 },
            { "api.key",           L"API 密钥", L"API KEY",      L"数据库加密存储",  ControlTerminalWindow::FieldKind::Text, L"",                          nullptr, true,  320 },
            { "api.org_id",        L"组织标识", L"ORG ID",       L"可留空",          ControlTerminalWindow::FieldKind::Text, L"",                          nullptr, false, 320 },
            { "api.model_default", L"默认模型", L"MODEL",        L"对话默认模型",    ControlTerminalWindow::FieldKind::Text, L"gpt-4o",                    nullptr, false, 280 },
        };

        // ===== 卡片 02：采样参数 =====
        const ControlTerminalWindow::FieldDef kSamplingFields[] = {
            { "sampling.temperature",       L"温度",     L"TEMPERATURE",      L"0 – 2",   ControlTerminalWindow::FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "sampling.top_p",             L"核采样",   L"TOP P",            L"0 – 1",   ControlTerminalWindow::FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "sampling.frequency_penalty", L"频率惩罚", L"FREQ PENALTY",     L"-2 – 2",  ControlTerminalWindow::FieldKind::Double, L"0.0", nullptr, false, 150 },
            { "sampling.presence_penalty",  L"存在惩罚", L"PRESENCE PENALTY", L"-2 – 2",  ControlTerminalWindow::FieldKind::Double, L"0.0", nullptr, false, 150 },
        };

        // ===== 卡片 03：流式与超时 =====
        const ControlTerminalWindow::FieldDef kStreamFields[] = {
            { "stream.enabled",         L"流式输出", L"STREAM",  L"逐字返回", ControlTerminalWindow::FieldKind::Bool, L"true", nullptr, false, 0 },
            { "stream.timeout_seconds", L"请求超时", L"TIMEOUT", L"秒",       ControlTerminalWindow::FieldKind::Int,  L"30",   nullptr, false, 150 },
        };

        // ===== 卡片 04：上下文 =====
        const ControlTerminalWindow::FieldDef kContextFields[] = {
            { "context.max_tokens", L"上下文上限", L"MAX TOKENS", L"token", ControlTerminalWindow::FieldKind::Int, L"4096", nullptr, false, 150 },
            { "context.max_rounds", L"记忆轮数",   L"MAX ROUNDS", L"轮",    ControlTerminalWindow::FieldKind::Int, L"30",   nullptr, false, 150 },
        };

        // ===== 卡片 05：主动触发 =====
        const ControlTerminalWindow::FieldDef kActiveFields[] = {
            { "active.interval_process",    L"进程巡检",     L"PROCESS",     L"秒 · -1 关闭", ControlTerminalWindow::FieldKind::Int,  L"60",   nullptr, false, 150 },
            { "active.interval_weather",    L"天气刷新",     L"WEATHER",     L"秒 · -1 关闭", ControlTerminalWindow::FieldKind::Int,  L"300",  nullptr, false, 150 },
            { "active.interval_screenshot", L"截屏巡检",     L"SCREENSHOT",  L"秒 · -1 关闭", ControlTerminalWindow::FieldKind::Int,  L"1800", nullptr, false, 150 },
            { "active.cooldown_seconds",    L"全局冷却",     L"COOLDOWN",    L"秒",           ControlTerminalWindow::FieldKind::Int,  L"480",  nullptr, false, 150 },
            { "active.ip_check_on_start",   L"启动时查 IP",  L"IP ON START", L"联网检测",     ControlTerminalWindow::FieldKind::Bool, L"true", nullptr, false, 0 },
            { "amap.api_key",               L"高德密钥",     L"AMAP KEY",    L"天气服务",     ControlTerminalWindow::FieldKind::Text, L"",     nullptr, true,  320 },
        };

        // ===== 卡片 06：模型与外观 =====
        const ControlTerminalWindow::FieldDef kModelFields[] = {
            { "model.default_id",      L"默认模型", L"MODEL ID", L"Live2D 名称", ControlTerminalWindow::FieldKind::Text,   L"Haru", nullptr, false, 240 },
            { "model.default_scale",   L"缩放",     L"SCALE",    L"倍",          ControlTerminalWindow::FieldKind::Double, L"1.0",  nullptr, false, 150 },
            { "model.default_opacity", L"不透明度", L"OPACITY",  L"0 – 1",       ControlTerminalWindow::FieldKind::Double, L"1.0",  nullptr, false, 150 },
        };

        // ===== 卡片 07：行为开关 =====
        const ControlTerminalWindow::FieldDef kBehaviorFields[] = {
            { "feature.always_on_top",       L"窗口置顶",     L"ALWAYS ON TOP", L"压住其他窗口", ControlTerminalWindow::FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.click_through",       L"鼠标穿透",     L"CLICK THROUGH", L"不接收点击",   ControlTerminalWindow::FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.drag_enabled",        L"允许拖拽",     L"DRAG ENABLED",  L"按住拖动",     ControlTerminalWindow::FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.auto_hide_fullscreen", L"全屏自动隐藏", L"AUTO HIDE",     L"观影游戏时",   ControlTerminalWindow::FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "ui.theme",                    L"界面主题",     L"UI THEME",      L"跟随系统",     ControlTerminalWindow::FieldKind::Enum, L"system", L"system|dark|light", false, 0 },
        };

        // ===== 卡片 08：附件与日志 =====
        const ControlTerminalWindow::FieldDef kStorageFields[] = {
            { "attachment.max_size_mb",    L"单件上限", L"MAX SIZE",    L"MB", ControlTerminalWindow::FieldKind::Int, L"20",  nullptr, false, 150 },
            { "attachment.total_quota_mb", L"总配额",   L"TOTAL QUOTA", L"MB", ControlTerminalWindow::FieldKind::Int, L"500", nullptr, false, 150 },
            { "attachment.retention_days", L"附件保留", L"RETENTION",   L"天", ControlTerminalWindow::FieldKind::Int, L"30",  nullptr, false, 150 },
            { "logs.retention_days",       L"日志保留", L"LOG RETENTION", L"天", ControlTerminalWindow::FieldKind::Int, L"30", nullptr, false, 150 },
        };

        // ===== 卡片 09：代理与更新 =====
        const ControlTerminalWindow::FieldDef kNetworkFields[] = {
            { "proxy.type",                  L"代理类型",     L"PROXY TYPE", L"出网方式",   ControlTerminalWindow::FieldKind::Enum, L"none", L"none|http|socks5", false, 0 },
            { "proxy.host",                  L"代理主机",     L"PROXY HOST", L"可留空",     ControlTerminalWindow::FieldKind::Text, L"",     nullptr,            false, 260 },
            { "proxy.port",                  L"代理端口",     L"PROXY PORT", L"0 = 不使用", ControlTerminalWindow::FieldKind::Int,  L"0",    nullptr,            false, 150 },
            { "update.auto_check",           L"自动检查更新", L"AUTO UPDATE", L"启动时静默", ControlTerminalWindow::FieldKind::Bool, L"true", nullptr,           false, 0 },
            { "update.check_interval_hours", L"检查间隔",     L"INTERVAL",   L"小时",       ControlTerminalWindow::FieldKind::Int,  L"24",   nullptr,            false, 150 },
        };

        const ControlTerminalWindow::SectionDef kSections[] = {
            { L"01", L"AI · 接口与鉴权", L"ENDPOINT", L"模型服务的接入点与凭据，密钥写入本地数据库。", kApiFields, 4 },
            { L"02", L"生成 · 采样参数", L"SAMPLING", L"温度与惩罚项直接决定回复的稳定度与发散度。", kSamplingFields, 4 },
            { L"03", L"流式与超时", L"STREAM", L"流式返回与单次请求的超时上限。", kStreamFields, 2 },
            { L"04", L"上下文与记忆", L"CONTEXT", L"送入模型的历史窗口上限。", kContextFields, 2 },
            { L"05", L"主动触发", L"PROACTIVE", L"无输入时按间隔主动唤起，冷却时间用于互相抑制。", kActiveFields, 6 },
            { L"06", L"模型与外观", L"LIVE2D", L"桌面模型的默认姿态与不透明度。", kModelFields, 3 },
            { L"07", L"行为开关", L"BEHAVIOR", L"窗口层级、穿透与整屏隐藏策略。", kBehaviorFields, 5 },
            { L"08", L"附件与日志", L"STORAGE", L"附件配额与本地保留周期。", kStorageFields, 4 },
            { L"09", L"代理与更新", L"NETWORK", L"出网代理与自动更新检查。", kNetworkFields, 5 },
        };

        const int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

    } // namespace

    // ============================================================
    // 生命周期
    // ============================================================

    ControlTerminalWindow& ControlTerminalWindow::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        // 资源释放由 Shutdown() 完成。
        static ControlTerminalWindow* instance = new ControlTerminalWindow();
        return *instance;
    }

    bool ControlTerminalWindow::Initialize(HINSTANCE hInstance) {
        if (hwnd_) return true;
        instance_ = hInstance;

        HDC screen = GetDC(nullptr);
        dpi_ = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (dpi_ <= 0) dpi_ = 96;

        if (!RegisterWindowClass(hInstance)) return false;
        if (!CreateTerminalWindow(hInstance)) return false;

        CreateFonts();
        inputBrush_ = CreateSolidBrush(kInputBg);

        LoadFromConfig();
        RebuildLayout();
        CreateFieldControls();
        SyncEditVisibility();

        LOG_INFO("ControlTerminal", "Control terminal window initialized.");
        return true;
    }

    void ControlTerminalWindow::Shutdown() {
        if (!hwnd_) return;
        DestroyFieldControls();
        ReleaseBackdrop();
        if (inputBrush_) {
            DeleteObject(inputBrush_);
            inputBrush_ = nullptr;
        }
        DestroyFonts();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        visible_ = false;
        LOG_INFO("ControlTerminal", "Control terminal window destroyed.");
    }

    void ControlTerminalWindow::OnSaved(std::function<void()> callback) {
        onSaved_ = std::move(callback);
    }

    void ControlTerminalWindow::Show(bool show) {
        if (!hwnd_) return;
        if (!show) {
            Hide();
            return;
        }

        if (!visible_) {
            LoadFromConfig();
            CreateFieldControls();
        }
        scrollY_ = 0;
        RebuildLayout();
        SyncEditVisibility();

        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        visible_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        LOG_DEBUG("ControlTerminal", "Control terminal shown.");
    }

    void ControlTerminalWindow::Hide() {
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        LOG_DEBUG("ControlTerminal", "Control terminal hidden.");
    }

    void ControlTerminalWindow::Toggle() {
        if (visible_) {
            Hide();
        }
        else {
            Show(true);
        }
    }

    // ============================================================
    // 窗口创建
    // ============================================================

    bool ControlTerminalWindow::RegisterWindowClass(HINSTANCE hInstance) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ControlTerminalWindow::WindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;
        return RegisterClassExW(&wc) != 0;
    }

    bool ControlTerminalWindow::CreateTerminalWindow(HINSTANCE hInstance) {
        const int width = Scale(kWinW);
        const int height = Scale(kWinH);

        int x = 0, y = 0;
        RECT workArea = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
            x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
            y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        }

        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            WINDOW_CLASS_NAME,
            L"控制终端",
            WS_POPUP | WS_CLIPCHILDREN,
            x, y, width, height,
            nullptr, nullptr, hInstance, this);

        if (!hwnd_) {
            LOG_ERROR("ControlTerminal", "Failed to create control terminal window.");
            return false;
        }
        return true;
    }

    // ============================================================
    // 字体与度量
    // ============================================================

    void ControlTerminalWindow::CreateFonts() {
        const wchar_t* sans = L"Microsoft YaHei UI";
        const wchar_t* mono = L"Cascadia Mono";

        auto make = [this](int pt, int weight, const wchar_t* face) {
            return CreateFontW(-Scale(pt), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, face);
        };

        fonts_.title = make(22, FW_SEMIBOLD, sans);
        fonts_.heading = make(15, FW_SEMIBOLD, sans);
        fonts_.label = make(11, FW_NORMAL, mono);
        fonts_.labelEn = make(8, FW_NORMAL, mono);
        fonts_.body = make(11, FW_NORMAL, sans);
        fonts_.mono = make(11, FW_NORMAL, mono);
        fonts_.monoSm = make(9, FW_NORMAL, mono);
        fonts_.monoXs = make(8, FW_NORMAL, mono);
        fonts_.button = make(12, FW_NORMAL, sans);
    }

    void ControlTerminalWindow::DestroyFonts() {
        HFONT* all[] = {
            &fonts_.title, &fonts_.heading, &fonts_.label, &fonts_.labelEn, &fonts_.body,
            &fonts_.mono, &fonts_.monoSm, &fonts_.monoXs, &fonts_.button
        };
        for (HFONT* font : all) {
            if (*font) {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
    }

    int ControlTerminalWindow::Scale(int value) const {
        return MulDiv(value, dpi_, 96);
    }

    int ControlTerminalWindow::ClientWidth() const {
        RECT rc = {};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        return rc.right - rc.left;
    }

    int ControlTerminalWindow::ClientHeight() const {
        RECT rc = {};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        return rc.bottom - rc.top;
    }

    int ControlTerminalWindow::ViewportTop() const {
        return Scale(kHeaderH);
    }

    int ControlTerminalWindow::ViewportHeight() const {
        return ClientHeight() - Scale(kHeaderH) - Scale(kFooterH);
    }

    // ============================================================
    // 布局
    // ============================================================

    void ControlTerminalWindow::RebuildLayout() {
        rows_.clear();
        cards_.clear();
        buttons_.clear();
        if (!hwnd_) return;

        const int width = ClientWidth();
        const int contentLeft = Scale(kMargin);
        const int contentRight = width - Scale(kMargin);
        const int innerLeft = contentLeft + Scale(kCardPadX);
        const int innerRight = contentRight - Scale(kCardPadX);
        const int labelW = Scale(kLabelW);
        const int controlX = innerLeft + labelW + Scale(14);
        const int noteRight = innerRight;
        const int noteLeft = noteRight - Scale(kNoteW);

        int y = Scale(kHeaderH) - scrollY_;

        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            const int titleH = Scale(28);
            const int subH = Scale(30);
            const int cardH = Scale(kCardPadY) * 2 + titleH + subH + sec.fieldCount * Scale(kRowH);

            CardLayout card;
            card.sec = &sec;
            card.rc = { contentLeft, y, contentRight, y + cardH };
            card.title = { innerLeft, y + Scale(kCardPadY), innerRight, y + Scale(kCardPadY) + titleH };
            card.sub = { innerLeft, card.title.bottom, innerRight, card.title.bottom + subH };
            card.rowCount = sec.fieldCount;

            int rowTop = card.sub.bottom;

            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                RowLayout row;
                row.def = &def;
                row.label = { innerLeft, rowTop, innerLeft + labelW, rowTop + Scale(kRowH) };
                row.note = { noteLeft, rowTop, noteRight, rowTop + Scale(kRowH) };

                const int centerY = rowTop + Scale(kRowH) / 2;
                if (def.kind == FieldKind::Bool) {
                    row.control = { controlX, centerY - Scale(7), controlX + Scale(14), centerY + Scale(7) };
                }
                else if (def.kind == FieldKind::Enum) {
                    std::vector<std::wstring> options = SplitOptions(def.options);
                    const int segmentW = Scale(74);
                    const int segmentH = Scale(26);
                    const int gap = Scale(6);
                    row.segmentCount = static_cast<int>(options.size());
                    if (row.segmentCount > 4) row.segmentCount = 4;
                    for (int k = 0; k < row.segmentCount; ++k) {
                        const int left = controlX + k * (segmentW + gap);
                        row.segments[k] = { left, centerY - segmentH / 2, left + segmentW, centerY + segmentH / 2 };
                    }
                    if (row.segmentCount > 0) row.control = row.segments[0];
                }
                else {
                    row.control = { controlX, centerY - Scale(kInputH) / 2, controlX + Scale(def.controlWidth), centerY + Scale(kInputH) / 2 };
                }

                auto editIt = edits_.find(def.key);
                row.edit = (editIt != edits_.end()) ? editIt->second : nullptr;

                rows_.push_back(row);
                rowTop += Scale(kRowH);
            }

            cards_.push_back(card);
            y += cardH + Scale(kCardGap);
        }

        contentHeight_ = (y + scrollY_) - Scale(kHeaderH) + Scale(kMargin);
        const int maxScroll = MaxInt(0, contentHeight_ - ViewportHeight());
        if (scrollY_ > maxScroll) {
            scrollY_ = maxScroll;
        }
        if (scrollY_ < 0) scrollY_ = 0;

        // 关闭按钮
        closeButton_ = { width - Scale(kMargin) - Scale(24), Scale(24), width - Scale(kMargin), Scale(24) + Scale(24) };

        // 页脚按钮（自右向左排布）
        const int footerTop = ClientHeight() - Scale(kFooterH);
        const int buttonY = footerTop + (Scale(kFooterH) - Scale(kBtnH)) / 2;
        struct ButtonSpec { int id; const wchar_t* text; bool primary; int width; };
        const ButtonSpec specs[] = {
            { ControlTerminalWindow::ID_BTN_SAVE,    L"保存",     true,  132 },
            { ControlTerminalWindow::ID_BTN_RELOAD,  L"重载",     false, 96 },
            { ControlTerminalWindow::ID_BTN_RESTORE, L"恢复默认", false, 116 },
        };
        int right = width - Scale(kMargin);
        for (const ButtonSpec& spec : specs) {
            ButtonLayout button;
            button.id = spec.id;
            button.text = spec.text;
            button.primary = spec.primary;
            button.rc = { right - Scale(spec.width), buttonY, right, buttonY + Scale(kBtnH) };
            buttons_.push_back(button);
            right = button.rc.left - Scale(12);
        }
    }

    void ControlTerminalWindow::DestroyFieldControls() {
        for (auto& pair : edits_) {
            if (pair.second) DestroyWindow(pair.second);
        }
        edits_.clear();
    }

    void ControlTerminalWindow::CreateFieldControls() {
        DestroyFieldControls();
        if (!hwnd_) return;

        int controlId = ID_FIRST_EDIT;
        for (const RowLayout& row : rows_) {
            const FieldDef& def = *row.def;
            if (def.kind != FieldKind::Text && def.kind != FieldKind::Int && def.kind != FieldKind::Double) {
                continue;
            }

            DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
            if (def.secret) style |= ES_PASSWORD;

            HWND edit = CreateWindowExW(
                0, L"EDIT", L"",
                style,
                row.control.left, row.control.top,
                row.control.right - row.control.left, row.control.bottom - row.control.top,
                hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId++)),
                instance_, nullptr);
            if (!edit) continue;

            SendMessageW(edit, EM_SETLIMITTEXT, 512, 0);
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.mono), TRUE);

            auto it = draft_.find(def.key);
            if (it != draft_.end()) {
                SetWindowTextW(edit, Utils::Utf8ToWide(it->second).c_str());
            }
            edits_[def.key] = edit;
        }
    }

    void ControlTerminalWindow::SyncEditVisibility() {
        if (!hwnd_) return;
        const int top = ViewportTop();
        const int bottom = top + ViewportHeight();

        for (const RowLayout& row : rows_) {
            if (!row.edit) continue;
            const bool inside = (row.control.top >= top) && (row.control.bottom <= bottom);
            if (inside) {
                SetWindowPos(row.edit, nullptr,
                    row.control.left, row.control.top,
                    row.control.right - row.control.left, row.control.bottom - row.control.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            else {
                ShowWindow(row.edit, SW_HIDE);
            }
        }
    }

    // ============================================================
    // 背景缓存
    // ============================================================

    void ControlTerminalWindow::ReleaseBackdrop() {
        if (backdropDc_ && backdropOld_) {
            SelectObject(backdropDc_, backdropOld_);
            backdropOld_ = nullptr;
        }
        if (backdropBitmap_) {
            DeleteObject(backdropBitmap_);
            backdropBitmap_ = nullptr;
        }
        if (backdropDc_) {
            DeleteDC(backdropDc_);
            backdropDc_ = nullptr;
        }
        backdropW_ = 0;
        backdropH_ = 0;
    }

    void ControlTerminalWindow::EnsureBackdrop() {
        const int width = ClientWidth();
        const int height = ClientHeight();
        if (width <= 0 || height <= 0) return;
        if (backdropDc_ && backdropW_ == width && backdropH_ == height) return;
        ReleaseBackdrop();

        HDC screen = GetDC(nullptr);
        backdropDc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!backdropDc_) return;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 自上而下
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        backdropBitmap_ = CreateDIBSection(backdropDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!backdropBitmap_ || !bits) {
            ReleaseBackdrop();
            return;
        }
        backdropOld_ = static_cast<HBITMAP>(SelectObject(backdropDc_, backdropBitmap_));
        backdropW_ = width;
        backdropH_ = height;

        // .bg：竖向渐变 + 两处冷蓝辉光 + 72px 网格 + 轻微暗角
        const float bg1[3] = { 10.0f, 21.0f, 48.0f };
        const float bg0[3] = { 6.0f, 11.0f, 25.0f };
        const float glowA[3] = { 112.0f, 160.0f, 255.0f };
        const float glowB[3] = { 84.0f, 132.0f, 240.0f };
        const float gridColor[3] = { 125.0f, 168.0f, 255.0f };
        const float vignette[3] = { 3.0f, 9.0f, 24.0f };

        uint32_t* pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < height; ++y) {
            float t = (height > 1) ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.0f;
            float k = t / 0.72f;
            if (k > 1.0f) k = 1.0f;
            const float baseR = bg1[0] + (bg0[0] - bg1[0]) * k;
            const float baseG = bg1[1] + (bg0[1] - bg1[1]) * k;
            const float baseB = bg1[2] + (bg0[2] - bg1[2]) * k;

            for (int x = 0; x < width; ++x) {
                float r = baseR, g = baseG, b = baseB;

                const float dxA = (static_cast<float>(x) - static_cast<float>(width) * 0.76f) / 1200.0f;
                const float dyA = (static_cast<float>(y) - static_cast<float>(height) * 0.08f) / 820.0f;
                float uA = 1.0f - std::sqrt(dxA * dxA + dyA * dyA) / 0.62f;
                if (uA > 0.0f) {
                    const float a = 0.20f * uA * uA;
                    r += (glowA[0] - r) * a;
                    g += (glowA[1] - g) * a;
                    b += (glowA[2] - b) * a;
                }

                const float dxB = (static_cast<float>(x) - static_cast<float>(width) * -0.04f) / 980.0f;
                const float dyB = (static_cast<float>(y) - static_cast<float>(height) * 1.12f) / 720.0f;
                float uB = 1.0f - std::sqrt(dxB * dxB + dyB * dyB) / 0.55f;
                if (uB > 0.0f) {
                    const float a = 0.16f * uB * uB;
                    r += (glowB[0] - r) * a;
                    g += (glowB[1] - g) * a;
                    b += (glowB[2] - b) * a;
                }

                // 网格（72px）按径向遮罩淡出
                if (x % 72 == 0 || y % 72 == 0) {
                    const float mx = (static_cast<float>(x) - static_cast<float>(width) * 0.5f) / (static_cast<float>(width) * 0.575f);
                    const float my = (static_cast<float>(y) - static_cast<float>(height) * 0.42f) / (static_cast<float>(height) * 0.475f);
                    const float md = std::sqrt(mx * mx + my * my);
                    float mask = 1.0f;
                    if (md >= 0.88f) {
                        mask = 0.0f;
                    }
                    else if (md > 0.40f) {
                        mask = (0.88f - md) / 0.48f;
                    }
                    const float a = 0.085f * 0.72f * mask;
                    if (a > 0.0f) {
                        r += (gridColor[0] - r) * a;
                        g += (gridColor[1] - g) * a;
                        b += (gridColor[2] - b) * a;
                    }
                }

                // 暗角
                const float vx = (static_cast<float>(x) - static_cast<float>(width) * 0.5f) / (static_cast<float>(width) * 0.6f);
                const float vy = (static_cast<float>(y) - static_cast<float>(height) * 0.4f) / (static_cast<float>(height) * 0.6f);
                const float vd = std::sqrt(vx * vx + vy * vy);
                if (vd > 0.52f) {
                    float a = (vd - 0.52f) / 0.48f;
                    if (a > 1.0f) a = 1.0f;
                    a *= 0.5f;
                    r += (vignette[0] - r) * a;
                    g += (vignette[1] - g) * a;
                    b += (vignette[2] - b) * a;
                }

                pixels[y * width + x] =
                    (static_cast<uint32_t>(255) << 24) |
                    (static_cast<uint32_t>(ClampInt(static_cast<int>(b + 0.5f), 0, 255)) << 16) |
                    (static_cast<uint32_t>(ClampInt(static_cast<int>(g + 0.5f), 0, 255)) << 8) |
                    static_cast<uint32_t>(ClampInt(static_cast<int>(r + 0.5f), 0, 255));
            }
        }
    }

    // ============================================================
    // 绘制
    // ============================================================

    void ControlTerminalWindow::Paint(HDC hdc) {
        EnsureBackdrop();
        if (backdropDc_) {
            BitBlt(hdc, 0, 0, ClientWidth(), ClientHeight(), backdropDc_, 0, 0, SRCCOPY);
        }
        else {
            RECT rc = { 0, 0, ClientWidth(), ClientHeight() };
            FillVerticalGradient(hdc, rc, kBgTop, kBgBottom);
        }

        PaintCards(hdc);
        PaintHeader(hdc);
        PaintFooter(hdc);
    }

    void ControlTerminalWindow::PaintHeader(HDC hdc) {
        SetBkMode(hdc, TRANSPARENT);
        const int width = ClientWidth();
        const int margin = Scale(kMargin);

        HGDIOBJ oldFont = SelectObject(hdc, fonts_.title);
        DrawSpacedText(hdc, L"控制终端", margin, Scale(24), Scale(3), kHeading);

        SelectObject(hdc, fonts_.monoSm);
        DrawSpacedText(hdc, L"UNNAMEDBOT · CONFIG TERMINAL", margin, Scale(60), Scale(2), kTextMuted);

        // 右上角关闭：两笔交叉线
        const bool closeHover = (hoveredButton_ == ID_BTN_CLOSE);
        const COLORREF closeColor = closeHover ? kIceHigh : kTextSecondary;
        const int closePad = Scale(7);
        HPEN pen = CreatePen(PS_SOLID, Scale(1) + 1, closeColor);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, closeButton_.left + closePad, closeButton_.top + closePad, nullptr);
        LineTo(hdc, closeButton_.right - closePad, closeButton_.bottom - closePad);
        MoveToEx(hdc, closeButton_.right - closePad, closeButton_.top + closePad, nullptr);
        LineTo(hdc, closeButton_.left + closePad, closeButton_.bottom - closePad);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        // 右侧状态：来源标签 + 同步状态
        SelectObject(hdc, fonts_.monoXs);
        DrawSpacedTextRight(hdc, L"CONFIG SOURCE", width - margin, Scale(50), Scale(2), kTextMuted);

        const COLORREF stateColor = dirty_ ? kAccent : kIce;
        const std::wstring stateText = dirty_
            ? std::wstring(L"未保存的修改")
            : std::wstring(L"已同步 · app.db");
        SelectObject(hdc, fonts_.monoSm);
        const int stateWidth = SpacedTextWidth(hdc, stateText, Scale(1));
        const int stateX = width - margin - stateWidth;
        DrawSpacedText(hdc, stateText, stateX, Scale(66), Scale(1), kTextSecondary);
        DrawDiamond(hdc, stateX - Scale(14), Scale(66) + Scale(7), Scale(4), stateColor);

        SelectObject(hdc, oldFont);
    }

    void ControlTerminalWindow::PaintCards(HDC hdc) {
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, 0, ViewportTop(), ClientWidth(), ViewportTop() + ViewportHeight());
        SetBkMode(hdc, TRANSPARENT);

        for (const CardLayout& card : cards_) {
            if (card.rc.bottom < ViewportTop() || card.rc.top > ViewportTop() + ViewportHeight()) continue;

            // .set-sec：竖向渐变面板 + 1px 冷蓝描边
            FillVerticalGradient(hdc, card.rc, kCardTop, kCardBottom);
            DrawBoxBorder(hdc, card.rc, kCardEdge);

            // .sec-title：序号（等宽冰蓝）+ 标题 + 英文小字
            const int titleY = card.title.top + Scale(4);
            HGDIOBJ oldFont = SelectObject(hdc, fonts_.monoSm);
            DrawSpacedText(hdc, card.sec->no, card.title.left, titleY + Scale(3), Scale(1), kIce);
            const int noWidth = SpacedTextWidth(hdc, card.sec->no, Scale(1));

            SelectObject(hdc, fonts_.heading);
            const int headingX = card.title.left + noWidth + Scale(12);
            DrawSpacedText(hdc, card.sec->title, headingX, titleY, Scale(1), kHeading);
            const int headingWidth = SpacedTextWidth(hdc, card.sec->title, Scale(1));

            SelectObject(hdc, fonts_.monoXs);
            DrawSpacedText(hdc, card.sec->titleEn, headingX + headingWidth + Scale(14), titleY + Scale(5), Scale(2), kTextMuted);

            // .sec-sub
            SelectObject(hdc, fonts_.body);
            SetTextColor(hdc, kTextMuted);
            RECT subRect = card.sub;
            subRect.top += Scale(4);
            DrawTextW(hdc, card.sec->sub, -1, &subRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            // .prow 字段行
            for (size_t i = 0; i < rows_.size(); ++i) {
                const RowLayout& row = rows_[i];
                if (row.def < card.sec->fields || row.def >= card.sec->fields + card.sec->fieldCount) continue;

                const bool first = (row.def == card.sec->fields);
                if (!first) {
                    DrawHRule(hdc, row.label.left, row.note.right, row.label.top, kHairline);
                }

                SelectObject(hdc, fonts_.label);
                DrawSpacedText(hdc, row.def->label, row.label.left, row.label.top + Scale(7), Scale(1), kTextSecondary);

                SelectObject(hdc, fonts_.labelEn);
                DrawSpacedText(hdc, row.def->labelEn, row.label.left, row.label.top + Scale(22), Scale(1), kTextMuted);

                const bool invalid = IsInvalid(*row.def);
                switch (row.def->kind) {
                case FieldKind::Bool: {
                    auto it = draft_.find(row.def->key);
                    const bool on = (it != draft_.end()) ? (it->second == "true") : false;

                    DrawBoxBorder(hdc, row.control, on ? kIce : kEdgeStrong);
                    if (on) {
                        RECT inner = { row.control.left + 1, row.control.top + 1, row.control.right - 1, row.control.bottom - 1 };
                        FillColor(hdc, inner, kIce);

                        HPEN checkPen = CreatePen(PS_SOLID, Scale(1) + 1, kCheckDark);
                        HGDIOBJ previous = SelectObject(hdc, checkPen);
                        const int left = row.control.left - Scale(1);
                        const int top = row.control.top - Scale(1);
                        MoveToEx(hdc, left + Scale(4), top + Scale(8), nullptr);
                        LineTo(hdc, left + Scale(7), top + Scale(11));
                        LineTo(hdc, left + Scale(12), top + Scale(4));
                        SelectObject(hdc, previous);
                        DeleteObject(checkPen);
                    }

                    SelectObject(hdc, fonts_.monoXs);
                    const std::wstring tag = on ? L"ON" : L"OFF";
                    DrawSpacedTextRight(hdc, tag, row.note.right, row.note.top + Scale(15), Scale(2),
                        on ? kIceMid : kTextMuted);
                    break;
                }
                case FieldKind::Enum: {
                    std::vector<std::wstring> options = SplitOptions(row.def->options);
                    auto it = draft_.find(row.def->key);
                    const std::wstring current = (it != draft_.end()) ? Utils::Utf8ToWide(it->second) : std::wstring();

                    for (int k = 0; k < row.segmentCount && k < static_cast<int>(options.size()); ++k) {
                        const bool active = (current == options[k]);
                        FillColor(hdc, row.segments[k], active ? kSegActive : kInputBg);
                        DrawBoxBorder(hdc, row.segments[k], active ? kIceMid : kCardEdge);

                        SelectObject(hdc, fonts_.monoXs);
                        SetTextColor(hdc, active ? kIceHigh : kTextMuted);
                        RECT textRect = row.segments[k];
                        textRect.top += Scale(1);
                        DrawTextW(hdc, options[k].c_str(), -1, &textRect,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                    break;
                }
                default: {
                    const bool focused = (row.edit && GetFocus() == row.edit);
                    DrawBoxBorder(hdc, row.control, invalid ? kFailMark : (focused ? kIceMid : kCardEdge));
                    break;
                }
                }

                if (!row.def->note) continue;
                SelectObject(hdc, fonts_.monoXs);
                SetTextColor(hdc, invalid ? kErrorText : kTextMuted);
                RECT noteRect = row.note;
                DrawTextW(hdc, row.def->note, -1, &noteRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        RestoreDC(hdc, saved);
    }

    void ControlTerminalWindow::PaintFooter(HDC hdc) {
        SetBkMode(hdc, TRANSPARENT);
        const int width = ClientWidth();
        const int footerTop = ClientHeight() - Scale(kFooterH);

        DrawHRule(hdc, 0, width, footerTop, kCardEdge);

        // 状态行（.statline）：成功=冰蓝 失败=失败色
        COLORREF statusColor = kTextMuted;
        if (statusKind_ == 1) statusColor = kIce;
        else if (statusKind_ == 2) statusColor = kFailText;

        HGDIOBJ oldFont = SelectObject(hdc, fonts_.monoSm);
        RECT statusRect = { Scale(kMargin), footerTop, 0, ClientHeight() };
        statusRect.right = buttons_.empty() ? width - Scale(kMargin) : buttons_.back().rc.left - Scale(20);
        SetTextColor(hdc, statusColor);
        DrawTextW(hdc, statusText_.c_str(), -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 按钮：主按钮（保存）与幽灵按钮
        for (const ButtonLayout& button : buttons_) {
            const bool hovered = (hoveredButton_ == button.id);
            const COLORREF fill = button.primary ? kPrimaryFill : kGhostFill;
            const COLORREF border = hovered ? kIceHigh : (button.primary ? kIceMid : kEdgeStrong);
            DrawCutCornerPanel(hdc, button.rc, Scale(8), fill, border);

            SelectObject(hdc, fonts_.button);
            SetTextColor(hdc, button.primary ? kButtonText : (hovered ? kIce : kTextSecondary));
            RECT textRect = button.rc;
            DrawTextW(hdc, button.text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, oldFont);
    }

    // ============================================================
    // 配置读写
    // ============================================================

    void ControlTerminalWindow::LoadFromConfig() {
        draft_.clear();
        loaded_.clear();
        invalidKeys_.clear();

        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                std::string raw = ConfigManager::Instance().GetString(def.key, Utils::WideToUtf8(def.dflt));
                draft_[def.key] = raw;
                loaded_[def.key] = raw;
            }
        }
        dirty_ = false;
        restoreArmedAt_ = 0;
    }

    std::wstring ControlTerminalWindow::CurrentValue(const FieldDef& def) const {
        if (def.kind == FieldKind::Text || def.kind == FieldKind::Int || def.kind == FieldKind::Double) {
            auto editIt = edits_.find(def.key);
            if (editIt != edits_.end() && editIt->second) {
                const int length = GetWindowTextLengthW(editIt->second);
                std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
                GetWindowTextW(editIt->second, buffer.data(), length + 1);
                buffer.resize(static_cast<size_t>(length));
                return buffer;
            }
        }
        auto it = draft_.find(def.key);
        if (it != draft_.end()) return Utils::Utf8ToWide(it->second);
        return def.dflt;
    }

    bool ControlTerminalWindow::IsInvalid(const FieldDef& def) const {
        return std::find(invalidKeys_.begin(), invalidKeys_.end(), std::string(def.key)) != invalidKeys_.end();
    }

    void ControlTerminalWindow::RecalcDirty() {
        bool dirty = false;
        for (int s = 0; s < kSectionCount && !dirty; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                auto loadedIt = loaded_.find(def.key);
                const std::string loadedValue = (loadedIt != loaded_.end()) ? loadedIt->second : std::string();
                if (Utils::WideToUtf8(CurrentValue(def)) != loadedValue) {
                    dirty = true;
                    break;
                }
            }
        }
        if (dirty != dirty_) {
            dirty_ = dirty;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    bool ControlTerminalWindow::ValidateValue(const FieldDef& def, const std::wstring& value, std::wstring& error) const {
        const std::string narrow = Utils::Trim(Utils::WideToUtf8(value));

        if (def.kind == FieldKind::Int) {
            if (narrow.empty()) {
                error = L"不能为空";
                return false;
            }
            for (char ch : narrow) {
                if (!isdigit(static_cast<unsigned char>(ch)) && ch != '-' && ch != '+') {
                    error = L"必须是整数";
                    return false;
                }
            }
            if (narrow.size() > 9) {
                error = L"数值过大";
                return false;
            }
            return true;
        }

        if (def.kind == FieldKind::Double) {
            if (narrow.empty()) {
                error = L"不能为空";
                return false;
            }
            char* end = nullptr;
            std::strtod(narrow.c_str(), &end);
            if (end == narrow.c_str() || (end && *end != '\0')) {
                error = L"必须是数字";
                return false;
            }
            return true;
        }

        if (def.kind == FieldKind::Enum) {
            std::vector<std::wstring> options = SplitOptions(def.options);
            if (std::find(options.begin(), options.end(), value) == options.end()) {
                error = L"取值非法";
                return false;
            }
            return true;
        }

        // 服务地址需为 http(s) 端点
        if (std::string(def.key) == "api.base_url" && !narrow.empty()) {
            if (narrow.rfind("http://", 0) != 0 && narrow.rfind("https://", 0) != 0) {
                error = L"需以 http(s):// 开头";
                return false;
            }
        }
        return true;
    }

    void ControlTerminalWindow::Save() {
        invalidKeys_.clear();

        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                const std::wstring value = CurrentValue(def);
                std::wstring error;
                if (!ValidateValue(def, value, error)) {
                    invalidKeys_.push_back(def.key);
                    continue;
                }

                const std::string narrow = Utils::Trim(Utils::WideToUtf8(value));
                switch (def.kind) {
                case FieldKind::Int:
                    ConfigManager::Instance().SetInt(def.key, std::atoi(narrow.c_str()));
                    break;
                case FieldKind::Double:
                    ConfigManager::Instance().SetDouble(def.key, std::strtod(narrow.c_str(), nullptr));
                    break;
                case FieldKind::Bool:
                    ConfigManager::Instance().SetBool(def.key, narrow == "true");
                    break;
                default:
                    ConfigManager::Instance().SetString(def.key, narrow);
                    break;
                }
            }
        }

        if (!invalidKeys_.empty()) {
            wchar_t message[256] = {};
            swprintf_s(message, L"校验失败 · %d 项未写入 · %s",
                static_cast<int>(invalidKeys_.size()), Utils::Utf8ToWide(invalidKeys_.front()).c_str());
            SetStatus(message, 2);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // 刷新草稿，使界面与刚写入的值保持一致
        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                const std::string value = Utils::Trim(Utils::WideToUtf8(CurrentValue(def)));
                draft_[def.key] = value;
                loaded_[def.key] = value;
            }
        }
        dirty_ = false;
        restoreArmedAt_ = 0;
        SetStatus(L"已保存 · " + NowString() + L" · 配置已写入 app.db", 1);
        InvalidateRect(hwnd_, nullptr, FALSE);

        if (onSaved_) {
            onSaved_();
        }
        LOG_INFO("ControlTerminal", "Configuration saved from control terminal.");
    }

    void ControlTerminalWindow::Reload() {
        invalidKeys_.clear();
        LoadFromConfig();
        DestroyFieldControls();
        RebuildLayout();
        CreateFieldControls();
        SyncEditVisibility();
        SetStatus(L"已重新载入 · " + NowString(), 0);
        InvalidateRect(hwnd_, nullptr, FALSE);
        LOG_DEBUG("ControlTerminal", "Configuration reloaded from database.");
    }

    void ControlTerminalWindow::RestoreDefaults() {
        const ULONGLONG now = GetTickCount64();
        if (restoreArmedAt_ == 0 || now - restoreArmedAt_ > 4000) {
            restoreArmedAt_ = now;
            SetStatus(L"再次点击「恢复默认」以确认清空并写入默认值", 2);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        restoreArmedAt_ = 0;
        ConfigManager::Instance().RestoreDefaults();
        invalidKeys_.clear();
        LoadFromConfig();
        DestroyFieldControls();
        RebuildLayout();
        CreateFieldControls();
        SyncEditVisibility();
        SetStatus(L"已恢复默认值 · " + NowString(), 1);
        InvalidateRect(hwnd_, nullptr, FALSE);

        if (onSaved_) {
            onSaved_();
        }
        LOG_INFO("ControlTerminal", "Configuration restored to defaults from control terminal.");
    }

    void ControlTerminalWindow::SetStatus(const std::wstring& text, int kind) {
        statusText_ = text;
        statusKind_ = kind;
    }

    std::wstring ControlTerminalWindow::NowString() const {
        std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        wchar_t buffer[32] = {};
        swprintf_s(buffer, L"%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        return buffer;
    }

    void ControlTerminalWindow::ScrollBy(int pixels) {
        const int maxScroll = MaxInt(0, contentHeight_ - ViewportHeight());
        const int target = ClampInt(scrollY_ + pixels, 0, maxScroll);
        if (target == scrollY_) return;
        scrollY_ = target;
        RebuildLayout();
        SyncEditVisibility();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ============================================================
    // 命中测试
    // ============================================================

    int ControlTerminalWindow::ButtonIndexAt(POINT pt) const {
        for (size_t i = 0; i < buttons_.size(); ++i) {
            if (PtInRect(&buttons_[i].rc, pt)) return static_cast<int>(i);
        }
        return -1;
    }

    int ControlTerminalWindow::BoolRowAt(POINT pt) const {
        for (size_t i = 0; i < rows_.size(); ++i) {
            const RowLayout& row = rows_[i];
            if (row.def->kind != FieldKind::Bool) continue;
            RECT hit = row.control;
            InflateRect(&hit, Scale(6), Scale(6));
            if (PtInRect(&hit, pt)) return static_cast<int>(i);
        }
        return -1;
    }

    int ControlTerminalWindow::EnumRowAt(POINT pt, int& optionIndex) const {
        for (size_t i = 0; i < rows_.size(); ++i) {
            const RowLayout& row = rows_[i];
            if (row.def->kind != FieldKind::Enum) continue;
            for (int k = 0; k < row.segmentCount; ++k) {
                if (PtInRect(&row.segments[k], pt)) {
                    optionIndex = k;
                    return static_cast<int>(i);
                }
            }
        }
        optionIndex = -1;
        return -1;
    }

    bool ControlTerminalWindow::IsHeaderDragArea(POINT pt) const {
        if (pt.y >= Scale(kHeaderH)) return false;
        if (PtInRect(&closeButton_, pt)) return false;
        return true;
    }

    // ============================================================
    // 消息处理
    // ============================================================

    LRESULT CALLBACK ControlTerminalWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ControlTerminalWindow* self = reinterpret_cast<ControlTerminalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ControlTerminalWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    LRESULT ControlTerminalWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            const int width = ClientWidth();
            const int height = ClientHeight();

            // 双缓冲，避免滚动闪烁
            HDC memoryDc = CreateCompatibleDC(hdc);
            HBITMAP buffer = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldBitmap = SelectObject(memoryDc, buffer);
            Paint(memoryDc);
            BitBlt(hdc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
            SelectObject(memoryDc, oldBitmap);
            DeleteObject(buffer);
            DeleteDC(memoryDc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            ReleaseBackdrop();
            RebuildLayout();
            SyncEditVisibility();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&closeButton_, pt)) return HTCLIENT;
            if (IsHeaderDragArea(pt)) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_SETCURSOR: {
            POINT pt = {};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int hoveredSegment = -1;
            const bool clickable = PtInRect(&closeButton_, pt) ||
                ButtonIndexAt(pt) >= 0 ||
                BoolRowAt(pt) >= 0 ||
                (const_cast<ControlTerminalWindow*>(this)->EnumRowAt(pt, hoveredSegment) >= 0);
            SetCursor(LoadCursor(nullptr, clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetFocus(hwnd);

            if (PtInRect(&closeButton_, pt)) {
                Hide();
                return 0;
            }

            const int buttonIndex = ButtonIndexAt(pt);
            if (buttonIndex >= 0) {
                switch (buttons_[buttonIndex].id) {
                case ID_BTN_SAVE: Save(); break;
                case ID_BTN_RELOAD: Reload(); break;
                case ID_BTN_RESTORE: RestoreDefaults(); break;
                default: break;
                }
                return 0;
            }

            const int boolRow = BoolRowAt(pt);
            if (boolRow >= 0) {
                const FieldDef& def = *rows_[boolRow].def;
                const bool on = (draft_[def.key] == "true");
                draft_[def.key] = on ? "false" : "true";
                RecalcDirty();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            int optionIndex = -1;
            const int enumRow = EnumRowAt(pt, optionIndex);
            if (enumRow >= 0 && optionIndex >= 0) {
                const FieldDef& def = *rows_[enumRow].def;
                std::vector<std::wstring> options = SplitOptions(def.options);
                if (optionIndex < static_cast<int>(options.size())) {
                    draft_[def.key] = Utils::WideToUtf8(options[optionIndex]);
                    RecalcDirty();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hovered = -1;
            if (PtInRect(&closeButton_, pt)) {
                hovered = ID_BTN_CLOSE;
            }
            else {
                const int index = ButtonIndexAt(pt);
                if (index >= 0) hovered = buttons_[index].id;
            }
            if (hovered != hoveredButton_) {
                hoveredButton_ = hovered;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollBy(-delta / WHEEL_DELTA * Scale(kRowH) * 2);
            return 0;
        }

        case WM_COMMAND: {
            const int notification = HIWORD(wParam);
            if (notification == EN_CHANGE || notification == EN_SETFOCUS || notification == EN_KILLFOCUS) {
                HWND edit = reinterpret_cast<HWND>(lParam);
                // 清除该字段的校验标记
                for (auto& pair : edits_) {
                    if (pair.second == edit) {
                        std::string key = pair.first;
                        invalidKeys_.erase(std::remove(invalidKeys_.begin(), invalidKeys_.end(), key), invalidKeys_.end());
                        break;
                    }
                }
                RecalcDirty();
                if (notification != EN_CHANGE) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                else {
                    RECT footer = { 0, ClientHeight() - Scale(kFooterH), ClientWidth(), ClientHeight() };
                    InvalidateRect(hwnd_, &footer, FALSE);
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                Hide();
                return 0;
            }
            if (ctrl && wParam == 'S') {
                Save();
                return 0;
            }
            if (wParam == VK_NEXT) {
                ScrollBy(ViewportHeight() - Scale(kRowH));
                return 0;
            }
            if (wParam == VK_PRIOR) {
                ScrollBy(-(ViewportHeight() - Scale(kRowH)));
                return 0;
            }
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kTextPrimary);
            SetBkColor(hdc, kInputBg);
            return reinterpret_cast<LRESULT>(inputBrush_);
        }

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace UnnamedBot
