#pragma once

#include <Windows.h>

#include <functional>
#include <string>

namespace UnnamedBot {

    struct UiTheme;

    // 无边框迷你输入条：贴着参照窗口弹出。
    //
    // 输入控件用**原生 EDIT**：输入法候选窗、组合键、剪贴板、无障碍支持全部由系统
    // 控件负责。自绘文本框或自建 WebView 都会把 IME 变成一堆边界问题，而这里只是
    // 「输入一句话」，不值得为此承担那些风险。
    class QuickInputBar {
    public:
        static QuickInputBar& Instance();

        bool Initialize(HINSTANCE hInstance);
        void Shutdown();

        // 面板式：深色底 + 细描边（快速模式下左键点圆点）。
        // onSubmit 在用户按 Enter 时以 UTF-8 文本回调（空文本不回调）。
        // 已有输入条时先关闭再重开。
        void Popup(HWND anchor, std::function<void(const std::string&)> onSubmit);

        // 下划线式：整条透明（色键抠底），只剩一条下划线与文字（右键人物）。
        // 右键的意义就在这里——不遮桌面，只在脚下拉一条线。
        void PopupUnderline(HWND anchor, std::function<void(const std::string&)> onSubmit);

        void Close();
        bool IsVisible() const;

        HWND GetHandle() const { return hwnd_; }

    private:
        // 两种外观：面板式沿用原来的深色小条；下划线式是右键用的极简形态。
        enum class Style {
            Panel,
            Underline,
        };

        QuickInputBar() = default;
        ~QuickInputBar() = default;
        QuickInputBar(const QuickInputBar&) = delete;
        QuickInputBar& operator=(const QuickInputBar&) = delete;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 输入控件的子类过程：EDIT 自己吃掉回车/ESC，宿主窗口收不到这些按键，
        // 只能在控件这一层拦下来再转给输入条（见 CreateEditControl）。
        static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static WNDPROC previousEditProc_;

        bool EnsureWindow();
        void CreateEditControl();
        void Show(HWND anchor, std::function<void(const std::string&)> onSubmit, Style style);
        // 下划线式要 WS_EX_LAYERED + 色键透明；面板式要摘掉，否则整窗会被抠掉
        void ApplyLayeredStyle();
        // 主题变了（普通 <-> ？？？）要换字体族/斜体，这里按需重建
        HFONT EnsureFont(const UiTheme& theme);
        // 画刷按颜色缓存：同一颜色反复建刷会漏 GDI 对象
        HBRUSH EnsureBrush(HBRUSH& brush, COLORREF& cached, COLORREF color);
        // 下划线式专用：把 EDIT 的文本与未上屏的组字串画到父窗口的 DC 上
        // （分层窗口不合成子控件，只能自己画）
        void DrawEditContent(HDC dc, const UiTheme& theme);
        // 下划线式专用：自己画的光标（EDIT 被缩到 1x1，它的系统光标没有意义）
        void DrawCaret(HDC dc, const UiTheme& theme, const RECT& textRect, int offsetX);

        // 依据参照窗口与工作区可用空间决定落点（物理像素）
        void ComputePosition(HWND anchor, int width, int height, int& x, int& y) const;
        void Submit();

        HWND hwnd_ = nullptr;
        HWND edit_ = nullptr;
        HINSTANCE instance_ = nullptr;
        bool visible_ = false;
        Style style_ = Style::Panel;
        int dpi_ = 96;

        HFONT font_ = nullptr;
        // 当前 font_ 对应的主题参数，不一致就重建
        const wchar_t* fontFamily_ = nullptr;
        bool fontItalic_ = false;

        HBRUSH backgroundBrush_ = nullptr;
        COLORREF backgroundBrushColor_ = CLR_INVALID;
        // 色键抠底用的纯黑刷：下划线式的「透明」全靠它
        HBRUSH keyBrush_ = nullptr;
        COLORREF keyBrushColor_ = CLR_INVALID;

        std::function<void(const std::string&)> onSubmit_;

        static const wchar_t* WINDOW_CLASS_NAME;
        static constexpr int IDC_EDIT = 3001;
        // 下划线式的周期重画（输入法组字期间 EDIT 不发 EN_CHANGE）
        static constexpr UINT_PTR IDT_REFRESH = 2;
    };

} // namespace UnnamedBot
