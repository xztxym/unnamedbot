#pragma once

#include <Windows.h>

#include <functional>
#include <string>

namespace UnnamedBot {

    // 无边框迷你输入条：左键单击圆点/人物时贴着参照窗口弹出。
    //
    // 输入控件用**原生 EDIT**：输入法候选窗、组合键、剪贴板、无障碍支持全部由系统
    // 控件负责。自绘文本框或自建 WebView 都会把 IME 变成一堆边界问题，而这里只是
    // 「输入一句话」，不值得为此承担那些风险。
    class QuickInputBar {
    public:
        static QuickInputBar& Instance();

        bool Initialize(HINSTANCE hInstance);
        void Shutdown();

        // 在 anchor 附近弹出输入条。onSubmit 在用户按 Enter 时以 UTF-8 文本回调
        // （空文本不回调）。已有输入条时先关闭再重开。
        void Popup(HWND anchor, std::function<void(const std::string&)> onSubmit);

        void Close();
        bool IsVisible() const;

        HWND GetHandle() const { return hwnd_; }

    private:
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
        // 依据参照窗口与工作区可用空间决定落点（物理像素）
        void ComputePosition(HWND anchor, int width, int height, int& x, int& y) const;
        void Submit();

        HWND hwnd_ = nullptr;
        HWND edit_ = nullptr;
        HINSTANCE instance_ = nullptr;
        bool visible_ = false;
        int dpi_ = 96;
        HFONT font_ = nullptr;
        HBRUSH backgroundBrush_ = nullptr;

        std::function<void(const std::string&)> onSubmit_;

        static const wchar_t* WINDOW_CLASS_NAME;
        static constexpr int IDC_EDIT = 3001;
    };

} // namespace UnnamedBot
