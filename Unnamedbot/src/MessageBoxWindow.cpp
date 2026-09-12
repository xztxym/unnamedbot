#include "../include/MessageBoxWindow.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <commctrl.h>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace UnnamedBot {

    const wchar_t* MessageBoxWindow::WINDOW_CLASS_NAME = L"UnnamedBotMessageBoxClass";

    MessageBoxWindow& MessageBoxWindow::Instance() {
        static MessageBoxWindow instance;
        return instance;
    }

    bool MessageBoxWindow::Initialize(HINSTANCE hInstance, HWND parentHwnd) {
        if (hwnd_) {
            LOG_WARN("MessageBoxWindow", "Already initialized.");
            return true;
        }

        parentHwnd_ = parentHwnd;
        instance_ = hInstance;

        // 注册窗口类
        if (!RegisterWindowClass(hInstance)) {
            LOG_ERROR("MessageBoxWindow", "Failed to register window class.");
            return false;
        }

        // 创建窗口
        if (!CreateMessageBoxWindow(hInstance, parentHwnd)) {
            LOG_ERROR("MessageBoxWindow", "Failed to create window.");
            return false;
        }

        // 恢复状态
        RestoreState();

        LOG_INFO("MessageBoxWindow", "Initialized.");
        return true;
    }

    void MessageBoxWindow::Shutdown() {
        if (hwnd_) {
            SaveState();
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        LOG_INFO("MessageBoxWindow", "Shut down.");
    }

    void MessageBoxWindow::Show(bool show) {
        if (!hwnd_) return;

        if (show) {
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            visible_ = true;
            UpdateLayout();
        }
        else {
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
        }
    }

    void MessageBoxWindow::Hide() {
        Show(false);
    }

    void MessageBoxWindow::Toggle() {
        Show(!visible_);
    }

    void MessageBoxWindow::AddMessage(
        const std::string& sender,
        const std::string& content,
        const std::string& timestamp,
        bool isUser
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string ts = timestamp.empty() ? Utils::GetCurrentTimeString() : timestamp;
        messages_.push_back(std::make_tuple(sender, content, ts, isUser));

        // 如果窗口可见，刷新显示
        if (visible_) {
            AddMessageToList(sender, content, ts, isUser);
        }
    }

    void MessageBoxWindow::ClearMessages() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();

        if (listMessages_) {
            SendMessageW(listMessages_, LB_RESETCONTENT, 0, 0);
        }
    }

    void MessageBoxWindow::SetPosition(int x, int y) {
        if (!hwnd_) return;
        posX_ = x;
        posY_ = y;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    void MessageBoxWindow::GetPosition(int& x, int& y) const {
        if (!hwnd_) {
            x = posX_;
            y = posY_;
            return;
        }
        RECT rect;
        GetWindowRect(hwnd_, &rect);
        x = rect.left;
        y = rect.top;
    }

    void MessageBoxWindow::SetSize(int width, int height) {
        if (!hwnd_) return;
        width_ = width;
        height_ = height;
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
        UpdateLayout();
    }

    void MessageBoxWindow::GetSize(int& width, int& height) const {
        if (!hwnd_) {
            width = width_;
            height = height_;
            return;
        }
        RECT rect;
        GetWindowRect(hwnd_, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }

    void MessageBoxWindow::SetOpacity(float opacity) {
        if (!hwnd_) return;
        opacity_ = std::clamp(opacity, 0.3f, 1.0f);
        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(opacity_ * 255), LWA_ALPHA);
    }

    void MessageBoxWindow::SetAlwaysOnTop(bool topmost) {
        if (!hwnd_) return;
        alwaysOnTop_ = topmost;
        SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    void MessageBoxWindow::SaveState() {
        ConfigManager::Instance().SetInt("messagebox.pos_x", posX_);
        ConfigManager::Instance().SetInt("messagebox.pos_y", posY_);
        ConfigManager::Instance().SetInt("messagebox.width", width_);
        ConfigManager::Instance().SetInt("messagebox.height", height_);
        ConfigManager::Instance().SetDouble("messagebox.opacity", opacity_);
        ConfigManager::Instance().SetBool("messagebox.always_on_top", alwaysOnTop_);
        LOG_DEBUG("MessageBoxWindow", "State saved.");
    }

    void MessageBoxWindow::RestoreState() {
        posX_ = ConfigManager::Instance().GetInt("messagebox.pos_x", 100);
        posY_ = ConfigManager::Instance().GetInt("messagebox.pos_y", 300);
        width_ = ConfigManager::Instance().GetInt("messagebox.width", 400);
        height_ = ConfigManager::Instance().GetInt("messagebox.height", 500);
        opacity_ = static_cast<float>(ConfigManager::Instance().GetDouble("messagebox.opacity", 0.95));
        alwaysOnTop_ = ConfigManager::Instance().GetBool("messagebox.always_on_top", false);

        if (hwnd_) {
            SetPosition(posX_, posY_);
            SetSize(width_, height_);
            SetOpacity(opacity_);
            SetAlwaysOnTop(alwaysOnTop_);
        }
        LOG_DEBUG("MessageBoxWindow", "State restored.");
    }

    void MessageBoxWindow::OnSendMessage(std::function<void(const std::string&)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        onSendMessage_ = callback;
    }

    void MessageBoxWindow::OnClose(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        onClose_ = callback;
    }

    void MessageBoxWindow::EnableAttachment(bool enable) {
        if (btnAttach_) {
            EnableWindow(btnAttach_, enable);
        }
    }

    void MessageBoxWindow::RefreshMessages() {
        if (!listMessages_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        SendMessageW(listMessages_, LB_RESETCONTENT, 0, 0);

        for (const auto& msg : messages_) {
            const std::string& sender = std::get<0>(msg);
            const std::string& content = std::get<1>(msg);
            const std::string& timestamp = std::get<2>(msg);
            bool isUser = std::get<3>(msg);

            std::string display = (isUser ? "> " : "") + sender + ": " + content + " (" + timestamp + ")";
            std::wstring wDisplay(display.begin(), display.end());
            SendMessageW(listMessages_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wDisplay.c_str()));
        }

        // 滚动到底部
        int count = SendMessageW(listMessages_, LB_GETCOUNT, 0, 0);
        if (count > 0) {
            SendMessageW(listMessages_, LB_SETCURSEL, count - 1, 0);
        }
    }

    // ============ 私有方法 ============

    bool MessageBoxWindow::RegisterWindowClass(HINSTANCE hInstance) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = WINDOW_CLASS_NAME;

        if (FindWindowW(WINDOW_CLASS_NAME, nullptr)) {
            return true;
        }

        if (!RegisterClassExW(&wc)) {
            LOG_ERROR("MessageBoxWindow", "Failed to register window class. Error: " + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    bool MessageBoxWindow::CreateMessageBoxWindow(HINSTANCE hInstance, HWND parentHwnd) {
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            WINDOW_CLASS_NAME,
            L"UnnamedBot - Chat",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            posX_, posY_,
            width_, height_,
            parentHwnd,
            nullptr,
            hInstance,
            this
        );

        if (!hwnd_) {
            LOG_ERROR("MessageBoxWindow", "Failed to create window. Error: " + std::to_string(GetLastError()));
            return false;
        }

        // 设置透明度
        SetOpacity(opacity_);

        // 创建控件
        CreateControls();

        // 默认隐藏
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;

        return true;
    }

    void MessageBoxWindow::CreateControls() {
        // 消息列表（标准 LISTBOX 控件，见下方 LB_* 消息；不需要 Rich Edit）

        // 消息列表
        listMessages_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_READONLY | ES_MULTILINE,
            5, 5, width_ - 10, height_ - 80,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_LIST_MESSAGES),
            instance_,
            nullptr
        );

        // 输入框
        editMessage_ = CreateWindowExW(
            0,
            L"EDIT",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
            5, height_ - 70, width_ - 90, 60,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_EDIT_MESSAGE),
            instance_,
            nullptr
        );

        // 发送按钮
        btnSend_ = CreateWindowExW(
            0,
            L"BUTTON",
            L"Send",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            width_ - 80, height_ - 70, 70, 28,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_BTN_SEND),
            instance_,
            nullptr
        );

        // 附件按钮
        btnAttach_ = CreateWindowExW(
            0,
            L"BUTTON",
            L"Attach",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            width_ - 80, height_ - 38, 70, 28,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_BTN_ATTACH),
            instance_,
            nullptr
        );

        // 清空按钮
        btnClear_ = CreateWindowExW(
            0,
            L"BUTTON",
            L"Clear",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            5, height_ - 35, 60, 25,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_BTN_CLEAR),
            instance_,
            nullptr
        );
    }

    void MessageBoxWindow::UpdateLayout() {
        if (!hwnd_) return;

        RECT rect;
        GetClientRect(hwnd_, &rect);
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;

        if (listMessages_) {
            SetWindowPos(listMessages_, nullptr, 5, 5, w - 10, h - 80, SWP_NOZORDER);
        }
        if (editMessage_) {
            SetWindowPos(editMessage_, nullptr, 5, h - 70, w - 90, 60, SWP_NOZORDER);
        }
        if (btnSend_) {
            SetWindowPos(btnSend_, nullptr, w - 80, h - 70, 70, 28, SWP_NOZORDER);
        }
        if (btnAttach_) {
            SetWindowPos(btnAttach_, nullptr, w - 80, h - 38, 70, 28, SWP_NOZORDER);
        }
        if (btnClear_) {
            SetWindowPos(btnClear_, nullptr, 5, h - 35, 60, 25, SWP_NOZORDER);
        }
    }

    void MessageBoxWindow::AddMessageToList(const std::string& sender, const std::string& content, const std::string& timestamp, bool isUser) {
        if (!listMessages_) return;

        std::string display = (isUser ? "> " : "") + sender + ": " + content + " (" + timestamp + ")";
        std::wstring wDisplay(display.begin(), display.end());
        SendMessageW(listMessages_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wDisplay.c_str()));

        // 滚动到底部
        int count = SendMessageW(listMessages_, LB_GETCOUNT, 0, 0);
        if (count > 0) {
            SendMessageW(listMessages_, LB_SETCURSEL, count - 1, 0);
        }
    }

    // ============ 窗口过程 ============

    LRESULT CALLBACK MessageBoxWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MessageBoxWindow* pWindow = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pWindow = reinterpret_cast<MessageBoxWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWindow));
        }
        else {
            pWindow = reinterpret_cast<MessageBoxWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pWindow) {
            return pWindow->HandleMessage(hwnd, msg, wParam, lParam);
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    LRESULT MessageBoxWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_SIZE: {
            UpdateLayout();
            return 0;
        }

        case WM_COMMAND: {
            WORD wmId = LOWORD(wParam);
            switch (wmId) {
            case IDC_BTN_SEND: {
                // 获取输入文本
                if (editMessage_) {
                    int len = GetWindowTextLengthW(editMessage_) + 1;
                    std::wstring wText(len, L'\0');
                    GetWindowTextW(editMessage_, &wText[0], len);
                    std::string text(wText.begin(), wText.end());
                    text = Utils::Trim(text);

                    if (!text.empty()) {
                        // 添加到消息列表
                        AddMessage("You", text, Utils::GetCurrentTimeString(), true);

                        // 清空输入框
                        SetWindowTextW(editMessage_, L"");

                        // 调用回调
                        if (onSendMessage_) {
                            onSendMessage_(text);
                        }
                    }
                }
                return 0;
            }

            case IDC_BTN_CLEAR: {
                ClearMessages();
                return 0;
            }

            case IDC_BTN_ATTACH: {
                // 打开文件选择对话框
                OPENFILENAMEW ofn = {};
                wchar_t fileName[MAX_PATH] = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = fileName;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST;
                ofn.lpstrTitle = L"Select files to attach";

                if (GetOpenFileNameW(&ofn)) {
                    LOG_DEBUG("MessageBoxWindow", std::string("File attached: ") + Utils::WideToUtf8(fileName));
                    // TODO: 处理附件
                }
                return 0;
            }

            case IDC_EDIT_MESSAGE: {
                if (HIWORD(wParam) == EN_UPDATE) {
                    // 可以在这里添加输入提示
                }
                return 0;
            }
            }
            break;
        }

        case WM_CLOSE: {
            if (onClose_) {
                onClose_();
            }
            Show(false);
            return 0;
        }

        case WM_DESTROY: {
            SaveState();
            return 0;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

} // namespace UnnamedBot