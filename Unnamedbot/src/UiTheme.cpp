#include "../include/UiTheme.h"

#include "../include/ConfigManager.h"
#include "../include/Utils.h"

#include <cstdio>

namespace UnnamedBot {

    namespace {

        // 「？？？」在配置里存的字面量（与 ControlTerminalWindow 的下拉项一致）。
        const wchar_t* const kYandereLabel = L"？？？";

        // 病娇主题：陈酒红 / 暖黑的面板 / 米白正文 + 楷体斜体。
        // 只改配色与字形，不改任何布局尺寸——换主题不该把版面挤坏。
        //
        // 配色取向（迭代过一次）：
        //   第一版是「纯黑底 + 高饱和血红」，在屏幕上很像报警灯——硬、刺、廉价。
        //   这一版把红色降一档饱和度（#FF2B4E → #D6405C）、底色从纯黑抬到
        //   带紫的深酒红（#15060C → #1E0C14），正文从粉白改成暖米白，
        //   描边也退到酒红而不是暗红。整体还是「血」，但更像酒，不是警报。
        UiTheme MakeYandereTheme() {
            UiTheme theme;
            theme.yandere = true;

            theme.accent = RGB(0xD6, 0x40, 0x5C);        // 主色：陈血红（玫瑰调，降过饱和）
            theme.accentSoft = RGB(0xE8, 0x8A, 0x9C);    // 柔和档：悬停文字 / 指示点
            theme.accentHi = RGB(0xF6, 0xC9, 0xD2);      // 极浅玫瑰：高亮文字
            theme.accentMid = RGB(0xA6, 0x2B, 0x45);     // 暗一档的红
            theme.edge = RGB(0x6B, 0x23, 0x34);          // 酒红描边（原来偏黑红，太沉）
            theme.edgeSoft = RGB(0x35, 0x15, 0x20);      // 发丝线
            theme.surface = RGB(0x1E, 0x0C, 0x14);       // 深酒红面板（不再是近黑）
            theme.surfaceDeep = RGB(0x14, 0x07, 0x0D);
            theme.text = RGB(0xF2, 0xE4, 0xE6);          // 暖米白正文
            // 次要文字：原来是 #A98C94，压在酒色面板上只有 ~4.5:1，小字发糊，抬亮一档
            theme.textDim = RGB(0xBF, 0xA3, 0xAB);

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
