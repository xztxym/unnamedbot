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