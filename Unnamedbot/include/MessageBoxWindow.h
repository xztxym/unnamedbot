#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>

namespace UnnamedBot {

    // 消息框窗口
    class MessageBoxWindow {
    public:
        static MessageBoxWindow& Instance();

        // 初始化
        bool Initialize(HINSTANCE hInstance, HWND parentHwnd);

        // 关闭
        void Shutdown();

        // 显示/隐藏
        void Show(bool show = true);
        void Hide();
        bool IsVisible() const { return visible_; }

        // 切换显示状态
        void Toggle();

        // 添加消息
        void AddMessage(
            const std::string& sender,
            const std::string& content,
            const std::string& timestamp = "",
            bool isUser = true
        );

        // 清空消息
        void ClearMessages();

        // 设置位置
        void SetPosition(int x, int y);
        void GetPosition(int& x, int& y) const;

        // 设置大小
        void SetSize(int width, int height);
        void GetSize(int& width, int& height) const;

        // 设置透明度
        void SetOpacity(float opacity);

        // 设置置顶
        void SetAlwaysOnTop(bool topmost);

        // 保存/恢复状态
        void SaveState();
        void RestoreState();

        // 获取窗口句柄
        HWND GetHandle() const { return hwnd_; }

        // 设置回调
        void OnSendMessage(std::function<void(const std::string&)> callback);
        void OnClose(std::function<void()> callback);

        // 添加附件按钮
        void EnableAttachment(bool enable);

        // 更新消息列表
        void RefreshMessages();

    private:
        MessageBoxWindow() = default;
        ~MessageBoxWindow() = default;
        MessageBoxWindow(const MessageBoxWindow&) = delete;
        MessageBoxWindow& operator=(const MessageBoxWindow&) = delete;

        // 创建窗口
        bool CreateWindow(HINSTANCE hInstance, HWND parentHwnd);

        // 注册窗口类
        bool RegisterWindowClass(HINSTANCE hInstance);

        // 窗口过程（静态）
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 窗口过程（非静态）
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // 创建子控件
        void CreateControls();

        // 更新控件位置
        void UpdateLayout();

        // 添加消息到列表
        void AddMessageToList(const std::string& sender, const std::string& content, const std::string& timestamp, bool isUser);

        // 窗口句柄
        HWND hwnd_ = nullptr;
        HWND parentHwnd_ = nullptr;
        HINSTANCE instance_ = nullptr;

        // 子控件句柄
        HWND editMessage_ = nullptr;
        HWND listMessages_ = nullptr;
        HWND btnSend_ = nullptr;
        HWND btnAttach_ = nullptr;
        HWND btnClear_ = nullptr;

        // 窗口属性
        int posX_ = 100;
        int posY_ = 300;
        int width_ = 400;
        int height_ = 500;
        float opacity_ = 0.95f;
        bool alwaysOnTop_ = false;
        bool visible_ = false;

        // 消息列表
        std::vector<std::tuple<std::string, std::string, std::string, bool>> messages_;
        mutable std::mutex mutex_;

        // 回调
        std::function<void(const std::string&)> onSendMessage_;
        std::function<void()> onClose_;

        // 窗口类名
        static const wchar_t* WINDOW_CLASS_NAME;

        // 控件ID
        static const int IDC_EDIT_MESSAGE = 1001;
        static const int IDC_LIST_MESSAGES = 1002;
        static const int IDC_BTN_SEND = 1003;
        static const int IDC_BTN_ATTACH = 1004;
        static const int IDC_BTN_CLEAR = 1005;
    };

} // namespace UnnamedBot