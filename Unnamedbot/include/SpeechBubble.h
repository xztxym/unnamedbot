#pragma once

#include <Windows.h>

#include <string>

namespace UnnamedBot {

    // 屏幕气泡：把一小段回复贴到屏幕上。
    //
    // 与 MessageBoxWindow（完整聊天窗，带编辑框与消息列表）职责不同，这里只负责
    // 「显示一段文本」，因此做成无边框、不激活、点击穿透的分层窗口：浮在桌面上方，
    // 既不抢焦点，也不挡住下面的窗口。
    //
    // 位置不是随机取点：始终以参照窗口（桌宠/圆点）为锚，按显示器工作区的可用空间
    // 依次尝试右侧→左侧→上方→下方，再按文本长度做确定性的小幅错位。因此气泡总是
    // 出现在人物/圆点旁边，且同一段文本总是落在同一处（可复现，不会满屏乱跳）。
    class SpeechBubble {
    public:
        static SpeechBubble& Instance();

        bool Initialize(HINSTANCE hInstance);
        void Shutdown();

        // 显示文本。anchor 为参照窗口，可为 nullptr（此时锚到主显示器的右上角）。
        // 重复调用会替换当前内容并重新计时。
        void Show(HWND anchor, const std::wstring& text);

        // 让气泡跟着鼠标指针出现，而不是永远贴在桌宠旁边：
        // 话是在「你」所在的位置说的，而不是在桌宠那边自言自语（？？？ 下尤其如此）。
        // 取不到指针位置时自动退回 Show 的行为。
        void ShowAtCursor(const std::wstring& text);

        // 按「当前活动窗口」定位：回复落在你正在用的那个窗口里，
        // 而不是按屏幕绝对坐标乱飘（多窗口 / 多显示器时尤其明显）。
        //   前台窗口是我们自己的（桌宠 / 配置中枢）→ 贴桌宠；
        //   是别人的窗口 → 落在那个窗口内、靠近指针处；指针不在其中则贴它右下角。
        void ShowNearActiveWindow(const std::wstring& text);

        // 按设置里的「回复吸附」决定气泡贴哪儿：
        // ui.reply_anchor = window（默认，贴活动窗口）| cursor（贴鼠标）
        void ShowAnchored(const std::wstring& text);

        void Hide();
        bool IsVisible() const;

        HWND GetHandle() const { return hwnd_; }

    private:
        SpeechBubble() = default;
        ~SpeechBubble() = default;
        SpeechBubble(const SpeechBubble&) = delete;
        SpeechBubble& operator=(const SpeechBubble&) = delete;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        bool EnsureWindow();
        // 文本换行后的外框尺寸（逻辑像素，含内边距）
        void MeasureText(const std::wstring& text, int& width, int& height) const;
        // 依据参照窗口与工作区可用空间决定落点（物理像素）
        void ComputePosition(HWND anchor, int width, int height, int& x, int& y) const;
        // 依据鼠标指针位置决定落点（物理像素）：贴在指针右下方，放不下就翻面
        void ComputePositionAtCursor(int width, int height, int& x, int& y) const;
        // 依据前台窗口决定落点（物理像素），落点被夹在目标窗口内部
        void ComputePositionNearActiveWindow(int width, int height, int& x, int& y) const;
        // 显示流程：两种落点方式共用（Show / ShowAtCursor 都走这里）
        // placement: 0=锚点窗口 1=鼠标指针 2=当前活动窗口
        void ShowInternal(HWND anchor, const std::wstring& text, int placement);
        // 绘制并提交到分层窗口
        void Render();
        void OnAutoHideTimer();

        // 分层窗口的绘制资源：尺寸变化时重建（字节格式为预乘 ARGB，见 Render）
        bool EnsureRenderTarget(int width, int height);
        void ReleaseRenderTarget();

        HWND hwnd_ = nullptr;
        HINSTANCE instance_ = nullptr;
        bool visible_ = false;
        int dpi_ = 96;

        // 当前内容与它的锚点
        std::wstring text_;
        HWND anchor_ = nullptr;

        // 气泡在屏幕上的落点（物理像素）：由 Show 决定，Render 直接使用，
        // 避免分层窗口先 SetWindowPos 再 UpdateLayeredWindow 两次移动
        int bubbleX_ = 0;
        int bubbleY_ = 0;

        // 分层窗口的 GDI 资源
        HFONT font_ = nullptr;
        // 当前 font_ 对应的主题参数（字体族/斜体），不一致就重建
        const wchar_t* fontFamily_ = nullptr;
        bool fontItalic_ = false;
        HDC memDc_ = nullptr;
        HBITMAP bitmap_ = nullptr;
        HBITMAP oldBitmap_ = nullptr;
        void* bitmapBits_ = nullptr;
        int bitmapWidth_ = 0;
        int bitmapHeight_ = 0;

        static const wchar_t* WINDOW_CLASS_NAME;
        static constexpr UINT_PTR IDT_AUTOHIDE = 1;
    };

} // namespace UnnamedBot
