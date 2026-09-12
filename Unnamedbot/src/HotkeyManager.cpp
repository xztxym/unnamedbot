#include "../include/HotkeyManager.h"
#include "../include/Logger.h"
#include <sstream>
#include <regex>
#include <algorithm>

namespace UnnamedBot {

    HotkeyManager& HotkeyManager::Instance() {
        static HotkeyManager instance;
        return instance;
    }

    bool HotkeyManager::Initialize(HWND hwnd) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("HotkeyManager", "Already initialized.");
            return true;
        }

        if (!hwnd) {
            LOG_ERROR("HotkeyManager", "Invalid window handle.");
            return false;
        }

        hwnd_ = hwnd;
        initialized_ = true;
        LOG_INFO("HotkeyManager", "Initialized.");
        return true;
    }

    void HotkeyManager::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);

        // 注销所有热键
        for (const auto& pair : hotkeys_) {
            ::UnregisterHotKey(hwnd_, pair.first);
        }
        hotkeys_.clear();
        initialized_ = false;
        LOG_INFO("HotkeyManager", "Shut down.");
    }

    bool HotkeyManager::RegisterHotkey(
        int id,
        const std::string& keyCombination,
        std::function<void()> callback,
        const std::string& description
    ) {
        UINT modifiers, key;
        if (!ParseKeyCombination(keyCombination, modifiers, key)) {
            LOG_ERROR("HotkeyManager", "Failed to parse key combination: " + keyCombination);
            return false;
        }
        return RegisterHotkey(id, modifiers, key, callback, description);
    }

    bool HotkeyManager::RegisterHotkey(
        int id,
        UINT modifiers,
        UINT key,
        std::function<void()> callback,
        const std::string& description
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("HotkeyManager", "Not initialized.");
            return false;
        }

        // 如果已存在，先注销
        if (hotkeys_.find(id) != hotkeys_.end()) {
            ::UnregisterHotKey(hwnd_, id);
        }

        // 注册热键
        if (!::RegisterHotKey(hwnd_, id, modifiers, key)) {
            DWORD error = GetLastError();
            LOG_ERROR("HotkeyManager", "Failed to register hotkey. Error: " + std::to_string(error));
            return false;
        }

        // 保存热键信息
        auto info = std::make_shared<HotkeyInfo>();
        info->id = id;
        info->modifiers = modifiers;
        info->key = key;
        info->callback = callback;
        info->description = description.empty() ? GetHotkeyDisplayString(id) : description;
        info->enabled = true;

        hotkeys_[id] = info;

        LOG_INFO("HotkeyManager", "Registered hotkey: " + info->description);
        return true;
    }

    bool HotkeyManager::UnregisterHotkey(int id) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            return false;
        }

        auto it = hotkeys_.find(id);
        if (it == hotkeys_.end()) {
            LOG_WARN("HotkeyManager", "Hotkey not found: " + std::to_string(id));
            return false;
        }

        if (!::UnregisterHotKey(hwnd_, id)) {
            DWORD error = GetLastError();
            LOG_ERROR("HotkeyManager", "Failed to unregister hotkey. Error: " + std::to_string(error));
            return false;
        }

        hotkeys_.erase(it);
        LOG_DEBUG("HotkeyManager", "Unregistered hotkey: " + std::to_string(id));
        return true;
    }

    bool HotkeyManager::EnableHotkey(int id, bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hotkeys_.find(id);
        if (it == hotkeys_.end()) {
            LOG_WARN("HotkeyManager", "Hotkey not found: " + std::to_string(id));
            return false;
        }

        it->second->enabled = enabled;

        if (enabled) {
            // 重新注册
            return ::RegisterHotKey(hwnd_, id, it->second->modifiers, it->second->key);
        }
        else {
            // 注销
            return ::UnregisterHotKey(hwnd_, id);
        }
    }

    void HotkeyManager::HandleHotkeyMessage(WPARAM wParam) {
        std::lock_guard<std::mutex> lock(mutex_);

        int id = static_cast<int>(wParam);
        auto it = hotkeys_.find(id);
        if (it != hotkeys_.end() && it->second->enabled && it->second->callback) {
            it->second->callback();
            LOG_DEBUG("HotkeyManager", "Hotkey triggered: " + it->second->description);
        }
    }

    bool HotkeyManager::IsHotkeyRegistered(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hotkeys_.find(id) != hotkeys_.end();
    }

    std::shared_ptr<HotkeyInfo> HotkeyManager::GetHotkeyInfo(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hotkeys_.find(id);
        return it != hotkeys_.end() ? it->second : nullptr;
    }

    std::map<int, std::shared_ptr<HotkeyInfo>> HotkeyManager::GetAllHotkeys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hotkeys_;
    }

    bool HotkeyManager::ParseKeyCombination(const std::string& keyCombination, UINT& modifiers, UINT& key) {
        modifiers = 0;
        key = 0;

        std::string str = keyCombination;
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);

        // 分割：移除空格，按+分割
        std::replace(str.begin(), str.end(), '+', ' ');
        std::istringstream iss(str);
        std::vector<std::string> parts;
        std::string part;
        while (iss >> part) {
            parts.push_back(part);
        }

        if (parts.empty()) {
            return false;
        }

        // 最后一个是按键
        std::string keyStr = parts.back();
        key = StringToVirtualKey(keyStr);
        if (key == 0) {
            return false;
        }

        // 前面的是修饰符
        for (size_t i = 0; i < parts.size() - 1; ++i) {
            std::string mod = parts[i];
            if (mod == "ctrl" || mod == "control") {
                modifiers |= MOD_CONTROL;
            }
            else if (mod == "alt") {
                modifiers |= MOD_ALT;
            }
            else if (mod == "shift") {
                modifiers |= MOD_SHIFT;
            }
            else if (mod == "win" || mod == "windows") {
                modifiers |= MOD_WIN;
            }
            else {
                LOG_WARN("HotkeyManager", "Unknown modifier: " + mod);
            }
        }

        return modifiers != 0 && key != 0;
    }

    std::string HotkeyManager::GetHotkeyDisplayString(int id) const {
        auto info = GetHotkeyInfo(id);
        if (!info) {
            return "Unknown Hotkey";
        }

        std::string result = ModifiersToString(info->modifiers);
        if (!result.empty()) {
            result += "+";
        }
        result += VirtualKeyToString(info->key);
        return result;
    }

    // ============ 辅助函数 ============

    UINT HotkeyManager::StringToVirtualKey(const std::string& keyStr) const {
        std::string str = keyStr;
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);

        // 字母和数字
        if (str.length() == 1 && str[0] >= 'a' && str[0] <= 'z') {
            return static_cast<UINT>(str[0] - 'a' + 'A');
        }
        if (str.length() == 1 && str[0] >= '0' && str[0] <= '9') {
            return static_cast<UINT>(str[0]);
        }

        // 特殊键
        static std::map<std::string, UINT> keyMap = {
            {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
            {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
            {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
            {"enter", VK_RETURN}, {"return", VK_RETURN},
            {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
            {"tab", VK_TAB},
            {"space", VK_SPACE},
            {"backspace", VK_BACK},
            {"delete", VK_DELETE}, {"del", VK_DELETE},
            {"insert", VK_INSERT}, {"ins", VK_INSERT},
            {"home", VK_HOME},
            {"end", VK_END},
            {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
            {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
            {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
            {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT},
            {"scrolllock", VK_SCROLL},
            {"pause", VK_PAUSE},
            {"numlock", VK_NUMLOCK},
            {"capslock", VK_CAPITAL}
        };

        auto it = keyMap.find(str);
        if (it != keyMap.end()) {
            return it->second;
        }

        return 0;
    }

    std::string HotkeyManager::VirtualKeyToString(UINT key) const {
        // 字母和数字
        if (key >= 'A' && key <= 'Z') {
            return std::string(1, static_cast<char>(key));
        }
        if (key >= '0' && key <= '9') {
            return std::string(1, static_cast<char>(key));
        }

        static std::map<UINT, std::string> keyMap = {
            {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F4, "F4"},
            {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F8, "F8"},
            {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
            {VK_RETURN, "Enter"},
            {VK_ESCAPE, "Esc"},
            {VK_TAB, "Tab"},
            {VK_SPACE, "Space"},
            {VK_BACK, "Backspace"},
            {VK_DELETE, "Delete"},
            {VK_INSERT, "Insert"},
            {VK_HOME, "Home"},
            {VK_END, "End"},
            {VK_PRIOR, "PageUp"},
            {VK_NEXT, "PageDown"},
            {VK_UP, "Up"},
            {VK_DOWN, "Down"},
            {VK_LEFT, "Left"},
            {VK_RIGHT, "Right"},
            {VK_SNAPSHOT, "PrintScreen"},
            {VK_SCROLL, "ScrollLock"},
            {VK_PAUSE, "Pause"},
            {VK_NUMLOCK, "NumLock"},
            {VK_CAPITAL, "CapsLock"}
        };

        auto it = keyMap.find(key);
        return it != keyMap.end() ? it->second : "Unknown";
    }

    std::string HotkeyManager::ModifiersToString(UINT modifiers) const {
        std::string result;
        if (modifiers & MOD_CONTROL) result += "Ctrl";
        if (modifiers & MOD_ALT) {
            if (!result.empty()) result += "+";
            result += "Alt";
        }
        if (modifiers & MOD_SHIFT) {
            if (!result.empty()) result += "+";
            result += "Shift";
        }
        if (modifiers & MOD_WIN) {
            if (!result.empty()) result += "+";
            result += "Win";
        }
        return result;
    }

} // namespace UnnamedBot