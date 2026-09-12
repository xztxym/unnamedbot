#pragma once

#include <Windows.h>
#include <string>
#include <map>
#include <functional>
#include <mutex>

namespace UnnamedBot {

    // 热键信息
    struct HotkeyInfo {
        int id = 0;
        UINT modifiers = 0;
        UINT key = 0;
        std::function<void()> callback;
        std::string description;
        bool enabled = true;
    };

    // 热键管理器
    class HotkeyManager {
    public:
        static HotkeyManager& Instance();

        // 初始化
        bool Initialize(HWND hwnd);

        // 关闭
        void Shutdown();

        // 注册热键
        bool RegisterHotkey(
            int id,
            const std::string& keyCombination,
            std::function<void()> callback,
            const std::string& description = ""
        );

        // 注册热键（使用修饰符和虚拟键码）
        bool RegisterHotkey(
            int id,
            UINT modifiers,
            UINT key,
            std::function<void()> callback,
            const std::string& description = ""
        );

        // 注销热键
        bool UnregisterHotkey(int id);

        // 启用/禁用热键
        bool EnableHotkey(int id, bool enabled);

        // 处理热键消息
        void HandleHotkeyMessage(WPARAM wParam);

        // 检查热键是否已注册
        bool IsHotkeyRegistered(int id) const;

        // 获取热键信息
        std::shared_ptr<HotkeyInfo> GetHotkeyInfo(int id) const;

        // 获取所有热键
        std::map<int, std::shared_ptr<HotkeyInfo>> GetAllHotkeys() const;

        // 解析键组合字符串
        bool ParseKeyCombination(const std::string& keyCombination, UINT& modifiers, UINT& key);

        // 获取热键显示文本
        std::string GetHotkeyDisplayString(int id) const;

    private:
        HotkeyManager() = default;
        ~HotkeyManager() = default;
        HotkeyManager(const HotkeyManager&) = delete;
        HotkeyManager& operator=(const HotkeyManager&) = delete;

        // 辅助：字符串转虚拟键码
        UINT StringToVirtualKey(const std::string& keyStr) const;

        // 辅助：虚拟键码转字符串
        std::string VirtualKeyToString(UINT key) const;

        // 辅助：修饰符转字符串
        std::string ModifiersToString(UINT modifiers) const;

        HWND hwnd_ = nullptr;
        std::map<int, std::shared_ptr<HotkeyInfo>> hotkeys_;
        mutable std::mutex mutex_;
        bool initialized_ = false;

        // 静态热键ID范围
        static const int HOTKEY_ID_BASE = 1000;
    };

} // namespace UnnamedBot