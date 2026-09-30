#pragma once

namespace UnnamedBot {

    // 配置终端的前端界面（自实现的 HTML/CSS/JS，单文档、零外部依赖）。
    //
    // 界面整体作为字符串编译进可执行文件，再用 WebView2 的 NavigateToString 装载，
    // 因此不需要把资源目录拷到输出目录，也不受工作目录影响。
    const char* ConfigWebUiHtml();

} // namespace UnnamedBot
