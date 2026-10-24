#pragma once

#include <Windows.h>

#include <string>

namespace UnnamedBot {

    // 界面主题：输入条、气泡、配置终端共用同一套取值。
    // 抽出来的目的是「？？？」人格启用时只改这一处，整套 UI 就一起换色换字体，
    // 不必在每个窗口里各写一份 if (yandere)。
    struct UiTheme {
        // 是不是「？？？」主题。各组件据此决定要不要加额外装饰（斜体、更硬的描边等）
        bool yandere = false;

        // ---- 配色（默认 = 原来的冷冰蓝深色主题，与配置终端一致）----
        COLORREF accent = RGB(0x87, 0xB2, 0xFF);        // 主色：下划线 / 描边 / 高亮
        COLORREF accentSoft = RGB(0xA8, 0xCE, 0xFF);    // 主色的柔和档（悬停文字）
        COLORREF accentHi = RGB(0xE0, 0xED, 0xFF);      // 更亮的主色
        COLORREF accentMid = RGB(0x86, 0xAE, 0xF2);     // 主色与底色之间
        COLORREF edge = RGB(0x5E, 0x7C, 0xA0);          // 面板描边
        COLORREF edgeSoft = RGB(0x17, 0x21, 0x3B);      // 发丝线 / 分隔线
        COLORREF surface = RGB(0x0E, 0x16, 0x2E);       // 面板底色
        COLORREF surfaceDeep = RGB(0x07, 0x0D, 0x1E);   // 更深的底色（输入条）
        COLORREF text = RGB(0xE8, 0xF1, 0xFF);          // 正文
        COLORREF textDim = RGB(0xA9, 0xBD, 0xE0);       // 次级文字

        // ---- 字体 ----
        const wchar_t* fontSans = L"Microsoft YaHei UI";
        const wchar_t* fontMono = L"Cascadia Mono";
        bool italic = false;
    };

    // 是否处于「？？？」：快速模式开启，且 quick.mode 选中了它。
    // 与 AppCore::IsYandereMode 同一判据，抽到这里供各 UI 组件共用（单一事实来源）。
    bool IsYandereModeActive();

    // 当前主题。每次调用都重新判定，改完配置立刻生效，不需要重启。
    UiTheme CurrentUiTheme();

    // COLORREF -> "#rrggbb"，给配置终端页面下发 CSS 变量用。
    std::string ToCssColor(COLORREF color);

} // namespace UnnamedBot
