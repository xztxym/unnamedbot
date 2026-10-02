#include "../include/UiTheme.h"

#include "../include/ConfigManager.h"
#include "../include/Utils.h"

#include <cstdio>

namespace UnnamedBot {

    namespace {

        // 「？？？」在配置里存的字面量（与 ControlTerminalWindow 的下拉项一致）。
        const wchar_t* const kYandereLabel = L"？？？";

        // 病娇主题：血红 / 近黑的面板 / 粉白正文 + 楷体斜体。
        // 只改配色与字形，不改任何布局尺寸——换主题不该把版面挤坏。
        UiTheme MakeYandereTheme() {
            UiTheme theme;
            theme.yandere = true;

            theme.accent = RGB(0xFF, 0x2B, 0x4E);        // 血红：下划线 / 描边
            theme.accentSoft = RGB(0xFF, 0x7D, 0x92);    // 柔和档：悬停文字 / 指示点
            theme.accentHi = RGB(0xFF, 0xD8, 0xDF);      // 极浅粉：高亮文字
            theme.accentMid = RGB(0xD8, 0x1B, 0x3F);     // 暗一档的红
            theme.edge = RGB(0x7A, 0x10, 0x30);          // 暗红描边
            theme.edgeSoft = RGB(0x3A, 0x0A, 0x18);      // 暗红发丝线
            theme.surface = RGB(0x15, 0x06, 0x0C);       // 近黑带红的面板
            theme.surfaceDeep = RGB(0x0C, 0x03, 0x07);
            theme.text = RGB(0xFF, 0xE9, 0xEE);          // 粉白正文
            theme.textDim = RGB(0xC0, 0x8A, 0x97);

            // 楷体：手写感、笔画带顿挫，比雅黑更「不对劲」。
            // 系统没装楷体时 GDI 会回退到默认字体，只是不好看，不会失败。
            theme.fontSans = L"KaiTi";
            theme.fontMono = L"KaiTi";
            theme.italic = true;
            return theme;
        }

    } // namespace

    bool IsYandereModeActive() {
        if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_QUICK_ENABLED, false)) {
            return false;
        }
        // 以 UTF-8 比较：配置里存的就是这个字符串（见 ControlTerminalWindow）
        return ConfigManager::Instance().GetString(ConfigManager::KEY_QUICK_MODE, "关闭")
            == Utils::WideToUtf8(kYandereLabel);
    }

    UiTheme CurrentUiTheme() {
        if (!IsYandereModeActive()) {
            return UiTheme();
        }
        return MakeYandereTheme();
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
