#include "../include/UiTheme.h"

#include <cstdio>

namespace UnnamedBot {

    // 「？？？」人格模式已经从产品里移除：哨兵文件 prompts/yandere.chr 已删除，
    // 这里再恒返回 false 做第二道保险——就算老配置里还留着 quick.mode = "？？？"，
    // 或者有人把 .chr 手工塞回 prompts 目录，界面也不会切成酒红主题。
    // 于是输入条 / 气泡 / 启动画面 / 配置终端始终是冷蓝那一套。
    bool IsYandereModeActive() {
        return false;
    }

    // 当前主题：只有默认的冷蓝主题一种。
    // 每次调用都返回默认值，改配置立刻生效，不需要重启。
    UiTheme CurrentUiTheme() {
        return UiTheme();
    }

    std::string ToCssColor(COLORREF color) {
        char buffer[8] = {};
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
            static_cast<unsigned>(GetRValue(color)),
            static_cast<unsigned>(GetGValue(color)),
            static_cast<unsigned>(GetBValue(color)));
        return std::string(buffer);
    }

} // namespace UnnamedBot
