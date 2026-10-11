#include "../include/TrayIcon.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <shellapi.h>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace UnnamedBot {

    TrayIcon& TrayIcon::Instance() {
        // 不随静态存储期销毁：窗口/渲染器在进程退出阶段仍会调用托盘（例如
        // 窗口析构触发 onClose 回调），此时静态对象早已被销毁。
        // 托盘图标由 Shutdown() 显式移除。
        static TrayIcon* instance = new TrayIcon();
        return *instance;
    }

    bool TrayIcon::Initialize(HWND hwnd, UINT callbackMessage, HICON icon) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("TrayIcon", "Tray icon already initialized.");
            return true;
        }

        if (!hwnd) {
            LOG_ERROR("TrayIcon", "Invalid window handle.");
            return false;
        }

        hwnd_ = hwnd;

        // 初始化NOTIFYICONDATA结构
        ZeroMemory(&nid_, sizeof(NOTIFYICONDATAW));
        nid_.cbSize = sizeof(NOTIFYICONDATAW);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uCallbackMessage = callbackMessage;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_STATE;
        nid_.dwState = NIS_SHAREDICON;
        nid_.dwStateMask = NIS_SHAREDICON;

        // 设置图标
        if (icon) {
            nid_.hIcon = icon;
        }
        else {
            // 使用默认图标
            nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        }

        // 设置提示文本
        wcscpy_s(nid_.szTip, L"UnnamedBot - Desktop Pet");

        // 添加到系统托盘
        if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
            LOG_ERROR("TrayIcon", "Failed to add tray icon. Error: " + std::to_string(GetLastError()));
            return false;
        }

        // 设置默认菜单
        SetDefaultMenuItems();

        initialized_ = true;
        visible_ = true;
        LOG_INFO("TrayIcon", "Tray icon initialized successfully.");
        return true;
    }

    void TrayIcon::Shutdown() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        // 移除托盘图标
        Shell_NotifyIconW(NIM_DELETE, &nid_);

        // 销毁菜单
        if (menuHandle_) {
            DestroyMenu(menuHandle_);
            menuHandle_ = nullptr;
        }

        initialized_ = false;
        LOG_INFO("TrayIcon", "Tray icon shut down.");
    }

    void TrayIcon::Show(bool show) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            LOG_WARN("TrayIcon", "Tray icon not initialized.");
            return;
        }

        visible_ = show;
        if (show) {
            Shell_NotifyIconW(NIM_ADD, &nid_);
        }
        else {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
        }
    }

    void TrayIcon::Hide() {
        Show(false);
    }

    void TrayIcon::SetTooltip(const std::wstring& tooltip) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        // szTip 只有 128 个宽字符：动态文本超出时必须截断，
        // wcscpy_s 在目标过小时会走无效参数处理并直接终止进程。
        wcsncpy_s(nid_.szTip, tooltip.c_str(), _TRUNCATE);
        nid_.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }

    void TrayIcon::SetIcon(HICON icon) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        nid_.hIcon = icon;
        nid_.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }

    void TrayIcon::ShowNotification(
        const std::wstring& title,
        const std::wstring& message,
        DWORD infoFlags,
        DWORD timeout
    ) {
        // 系统弹窗总开关（设置 → 通知）：关掉后所有托盘气泡都不再弹，
        // 桌宠自己的气泡与控制中枢不受影响。
        if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_NOTIFY_ENABLED, true)) {
            LOG_DEBUG("TrayIcon", "System notifications are disabled by config; skipped.");
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            LOG_WARN("TrayIcon", "Tray icon not initialized.");
            return;
        }

        // 使用新版通知（Windows 10+）
        NOTIFYICONDATAW nid = nid_;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = infoFlags;
        nid.uTimeout = timeout;

        // szInfoTitle/szInfo 分别只有 64/256 个宽字符。这里的内容是动态的
        // （例如消息正文可能上千字），必须截断：wcscpy_s 在目标过小时会触发
        // 无效参数处理，默认处理器直接 __fastfail 结束进程（0xC0000409）。
        wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);

        // 添加GUID以支持Windows 10通知
        // 简单起见，使用当前时间作为唯一ID
        nid.uID = ++notificationId_;

        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            LOG_ERROR("TrayIcon", "Failed to show notification. Error: " + std::to_string(GetLastError()));
        }
        else {
            LOG_DEBUG("TrayIcon", "Notification shown: " + Utils::WideToUtf8(title) + " - " + Utils::WideToUtf8(message));
        }
    }

    void TrayIcon::ShowPet(bool show) {
        petVisible_ = show;
        if (onShowPet_ && show) {
            onShowPet_();
        }
        else if (onHidePet_ && !show) {
            onHidePet_();
        }
    }

    void TrayIcon::TogglePetVisibility() {
        ShowPet(!petVisible_);
    }

    // ============ 菜单管理 ============

    void TrayIcon::SetMenuItems(const std::vector<TrayMenuItem>& items) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        menuItems_ = items;
    }

    void TrayIcon::AddMenuItem(const TrayMenuItem& item) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        menuItems_.push_back(item);
    }

    void TrayIcon::InsertMenuItem(int index, const TrayMenuItem& item) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (index < 0 || index > static_cast<int>(menuItems_.size())) {
            menuItems_.push_back(item);
        }
        else {
            menuItems_.insert(menuItems_.begin() + index, item);
        }
    }

    void TrayIcon::RemoveMenuItem(int id) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        menuItems_.erase(
            std::remove_if(menuItems_.begin(), menuItems_.end(),
                [id](const TrayMenuItem& item) { return item.id == id; }),
            menuItems_.end()
        );
    }

    void TrayIcon::ClearMenu() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        menuItems_.clear();
    }

    void TrayIcon::UpdateMenuItem(int id, const std::wstring& text, bool checked, bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto& item : menuItems_) {
            if (item.id == id) {
                item.text = text;
                item.isChecked = checked;
                item.isEnabled = enabled;
                break;
            }
        }
    }

    void TrayIcon::ShowContextMenu() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            LOG_WARN("TrayIcon", "Tray icon not initialized.");
            return;
        }

        HMENU menu = CreateMenu();
        if (!menu) {
            LOG_ERROR("TrayIcon", "Failed to create context menu.");
            return;
        }

        // 获取鼠标位置
        POINT pt;
        GetCursorPos(&pt);

        // 设置窗口为前台窗口，以便菜单能正确响应
        SetForegroundWindow(hwnd_);

        // 显示菜单
        int cmd = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
            pt.x,
            pt.y,
            0,
            hwnd_,
            nullptr
        );

        // 处理菜单命令
        if (cmd > 0) {
            OnMenuCommand(cmd);
        }

        // 销毁菜单
        DestroyMenu(menu);

        // 发送空消息以清理菜单状态
        PostMessage(hwnd_, WM_NULL, 0, 0);
    }

    // ============ 事件处理 ============

    void TrayIcon::HandleTrayMessage(WPARAM wParam, LPARAM lParam) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        // 检查是否是我们的托盘图标
        if (wParam != nid_.uID) {
            return;
        }

        switch (lParam) {
        case WM_LBUTTONDBLCLK: {
            // 双击托盘图标
            if (onTrayIconDoubleClick_) {
                onTrayIconDoubleClick_();
            }
            // 默认行为：切换消息框显示
            if (onOpenMessageBox_) {
                onOpenMessageBox_();
            }
            break;
        }

        case WM_RBUTTONUP: {
            // 右键点击 - 显示上下文菜单
            ShowContextMenu();
            break;
        }

        case WM_MOUSEMOVE: {
            // 鼠标悬停（可用来更新提示文本）
            break;
        }

        case NIN_BALLOONUSERCLICK: {
            // 点击通知气球
            LOG_DEBUG("TrayIcon", "Notification balloon clicked.");
            break;
        }

        default:
            break;
        }
    }

    void TrayIcon::OnMenuItemClick(std::function<void(int menuId)> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onMenuItemClick_ = callback;
    }

    void TrayIcon::OnTrayIconDoubleClick(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onTrayIconDoubleClick_ = callback;
    }

    void TrayIcon::OnShowPet(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onShowPet_ = callback;
    }

    void TrayIcon::OnHidePet(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onHidePet_ = callback;
    }

    void TrayIcon::OnOpenMessageBox(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onOpenMessageBox_ = callback;
    }

    void TrayIcon::OnOpenConfig(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onOpenConfig_ = callback;
    }

    void TrayIcon::OnSwitchModel(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onSwitchModel_ = callback;
    }

    void TrayIcon::OnExit(std::function<void()> callback) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        onExit_ = callback;
    }

    // ============ 私有方法 ============

    // 调用者需已持有 mutex_（Initialize 在持锁状态下调用）
    void TrayIcon::SetDefaultMenuItems() {
        menuItems_.clear();

        // 显示/隐藏桌宠
        TrayMenuItem togglePet;
        togglePet.id = MENU_ID_TOGGLE_PET;
        togglePet.text = L"显示桌宠";
        togglePet.isChecked = true;
        togglePet.isEnabled = true;
        menuItems_.push_back(togglePet);

        // 消息框
        TrayMenuItem messageBox;
        messageBox.id = MENU_ID_OPEN_MESSAGEBOX;
        messageBox.text = L"打开消息框";
        messageBox.isEnabled = true;
        menuItems_.push_back(messageBox);

        // 分隔符
        TrayMenuItem sep1;
        sep1.id = MENU_ID_SEPARATOR;
        sep1.isSeparator = true;
        menuItems_.push_back(sep1);

        // 切换模型
        TrayMenuItem switchModel;
        switchModel.id = MENU_ID_SWITCH_MODEL;
        switchModel.text = L"切换模型";
        switchModel.isEnabled = true;
        menuItems_.push_back(switchModel);

        // 配置
        TrayMenuItem config;
        config.id = MENU_ID_OPEN_CONFIG;
        config.text = L"配置";
        config.isEnabled = true;
        menuItems_.push_back(config);

        // 分隔符
        TrayMenuItem sep2;
        sep2.id = MENU_ID_SEPARATOR + 1;
        sep2.isSeparator = true;
        menuItems_.push_back(sep2);

        // 退出
        TrayMenuItem exit;
        exit.id = MENU_ID_EXIT;
        exit.text = L"退出";
        exit.isEnabled = true;
        menuItems_.push_back(exit);
    }

    HMENU TrayIcon::CreateMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return nullptr;
        }

        for (const auto& item : menuItems_) {
            if (item.isSeparator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }

            UINT flags = MF_STRING;
            if (!item.isEnabled) {
                flags |= MF_GRAYED;
            }
            if (item.isChecked) {
                flags |= MF_CHECKED;
            }

            // 特殊处理"显示桌宠"菜单项
            if (item.id == MENU_ID_TOGGLE_PET) {
                std::wstring text = petVisible_ ? L"隐藏桌宠" : L"显示桌宠";
                AppendMenuW(menu, flags, item.id, text.c_str());
            }
            else {
                AppendMenuW(menu, flags, item.id, item.text.c_str());
            }
        }

        return menu;
    }

    bool TrayIcon::UpdateTrayIcon(DWORD message) {
        if (!initialized_) {
            return false;
        }

        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        if (message != 0) {
            nid_.uCallbackMessage = message;
        }

        return Shell_NotifyIconW(NIM_MODIFY, &nid_) != FALSE;
    }

    void TrayIcon::OnMenuCommand(int menuId) {
        // 调用通用回调
        if (onMenuItemClick_) {
            onMenuItemClick_(menuId);
        }

        // 处理内置菜单命令
        switch (menuId) {
        case MENU_ID_TOGGLE_PET: {
            TogglePetVisibility();
            // 更新菜单项文本
            UpdateMenuItem(MENU_ID_TOGGLE_PET, petVisible_ ? L"隐藏桌宠" : L"显示桌宠");
            break;
        }

        case MENU_ID_OPEN_MESSAGEBOX: {
            if (onOpenMessageBox_) {
                onOpenMessageBox_();
            }
            break;
        }

        case MENU_ID_OPEN_CONFIG: {
            if (onOpenConfig_) {
                onOpenConfig_();
            }
            break;
        }

        case MENU_ID_SWITCH_MODEL: {
            if (onSwitchModel_) {
                onSwitchModel_();
            }
            break;
        }

        case MENU_ID_EXIT: {
            if (onExit_) {
                onExit_();
            }
            break;
        }

        default: {
            // 自定义菜单项
            LOG_DEBUG("TrayIcon", "Custom menu command: " + std::to_string(menuId));
            break;
        }
        }
    }

} // namespace UnnamedBot