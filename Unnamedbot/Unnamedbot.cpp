#include "include/AppCore.h"
#include "include/Logger.h"
#include <Windows.h>

using namespace UnnamedBot;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 设置控制台输出（Debug模式下）
#ifdef _DEBUG
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
#endif

    // 初始化应用程序
    auto& app = AppCore::Instance();
    if (!app.Initialize(hInstance, nCmdShow)) {
        LOG_ERROR("Main", "Failed to initialize application.");
        // 初始化失败时窗口尚未显示，静默退出只会让用户看到「闪退」，
        // 因此这里给出可见提示并指向日志。
        MessageBoxW(nullptr,
            L"UnnamedBot 初始化失败，程序即将退出。\n请查看 logs 目录下最新日志以获取详细信息。",
            L"UnnamedBot", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 运行应用程序
    int result = app.Run();

    // 关闭应用程序
    app.Shutdown();

#ifdef _DEBUG
    FreeConsole();
#endif

    return result;
}