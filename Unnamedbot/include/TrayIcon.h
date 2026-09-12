#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>

namespace UnnamedBot {

    // 托盘菜单项
    struct TrayMenuItem {
        int id;
        std::wstring text;
        bool isSeparator = false;
        bool isChecked = false;
        bool isEnabled = true;
        std::function<void()> callback;
    };

    // 托盘图标管理器
    class TrayIcon {
    public:
        static TrayIcon& Instance();

        // 初始化托盘图标
        bool Initialize(HWND hwnd, UINT callbackMessage, HICON icon = nullptr);

        // 销毁托盘图标
        void Shutdown();

        // 显示/隐藏托盘图标
        void Show(bool show = true);
        void Hide();

        // 设置提示文本
        void SetTooltip(const std::wstring& tooltip);

        // 设置图标
        void SetIcon(HICON icon);

        // 显示通知气球
        void ShowNotification(
            const std::wstring& title,
            const std::wstring& message,
            DWORD infoFlags = NIIF_INFO,
            DWORD timeout = 5000
        );

        // 显示/隐藏桌宠
        void ShowPet(bool show);

        // 切换桌宠显示状态
        void TogglePetVisibility();

        // 是否可见
        bool IsPetVisible() const { return petVisible_; }

        // ============ 菜单管理 ============

        // 设置菜单项
        void SetMenuItems(const std::vector<TrayMenuItem>& items);

        // 添加菜单项
        void AddMenuItem(const TrayMenuItem& item);

        // 插入菜单项
        void InsertMenuItem(int index, const TrayMenuItem& item);

        // 移除菜单项
        void RemoveMenuItem(int id);

        // 清空菜单
        void ClearMenu();

        // 更新菜单项状态
        void UpdateMenuItem(int id, const std::wstring& text, bool checked = false, bool enabled = true);

        // 弹出菜单（显示右键菜单）
        void ShowContextMenu();

        // ============ 事件处理 ============

        // 处理托盘消息（由窗口过程调用）
        void HandleTrayMessage(WPARAM wParam, LPARAM lParam);

        // 设置回调
        void OnMenuItemClick(std::function<void(int menuId)> callback);
        void OnTrayIconDoubleClick(std::function<void()> callback);
        void OnShowPet(std::function<void()> callback);
        void OnHidePet(std::function<void()> callback);
        void OnOpenMessageBox(std::function<void()> callback);
        void OnOpenConfig(std::function<void()> callback);
        void OnSwitchModel(std::function<void()> callback);
        void OnExit(std::function<void()> callback);

    private:
        TrayIcon() = default;
        ~TrayIcon() = default;
        TrayIcon(const TrayIcon&) = delete;
        TrayIcon& operator=(const TrayIcon&) = delete;

        // 创建菜单
        HMENU CreateMenu();

        // 创建默认菜单项（调用者需已持有 mutex_）
        void SetDefaultMenuItems();

        // 更新托盘图标
        bool UpdateTrayIcon(DWORD message);

        // 菜单消息处理
        void OnMenuCommand(int menuId);

        // 窗口句柄
        HWND hwnd_ = nullptr;

        // 托盘图标数据
        NOTIFYICONDATAW nid_ = {};
        bool initialized_ = false;
        bool visible_ = true;
        bool petVisible_ = true;

        // 菜单项
        std::vector<TrayMenuItem> menuItems_;
        HMENU menuHandle_ = nullptr;

        // 菜单ID范围
        static const int MENU_ID_BASE = 1000;
        static const int MENU_ID_SHOW_PET = 1001;
        static const int MENU_ID_HIDE_PET = 1002;
        static const int MENU_ID_TOGGLE_PET = 1003;
        static const int MENU_ID_OPEN_MESSAGEBOX = 1004;
        static const int MENU_ID_OPEN_CONFIG = 1005;
        static const int MENU_ID_SWITCH_MODEL = 1006;
        static const int MENU_ID_SEPARATOR = 1007;
        static const int MENU_ID_EXIT = 1008;

        // 回调函数
        std::function<void(int)> onMenuItemClick_;
        std::function<void()> onTrayIconDoubleClick_;
        std::function<void()> onShowPet_;
        std::function<void()> onHidePet_;
        std::function<void()> onOpenMessageBox_;
        std::function<void()> onOpenConfig_;
        std::function<void()> onSwitchModel_;
        std::function<void()> onExit_;

        // 线程安全
        mutable std::mutex mutex_;

        // 通知ID（用于区分多个通知）
        int notificationId_ = 0;
    };

} // namespace UnnamedBot