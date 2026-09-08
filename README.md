

# Unnamedbot

## 项目简介

本项目是一个基于 **Live2D Cubism SDK** 的原生应用程序项目，旨在利用 Live2D 核心技术进行动态模型（如虚拟主播/Vtuber 形象）的渲染与交互。

### 主要技术栈

*   **Live2D Cubism SDK**: 版本 `5-r.5`，提供核心渲染能力。
*   **多平台支持**: 基于 SDK 架构，支持 Windows, macOS, Linux, Android, iOS, HarmonyOS 等主流操作系统。
*   **多渲染后端**: 集成了业界主流图形接口，包括 **OpenGL / OpenGL ES**, **DirectX 9 / 11**, **Metal** 以及 **Vulkan**。
*   **构建工具**: 使用 **CMake** 进行统一的跨平台项目管理。

## 目录结构

```text
Unnamedbot.slnx                 # 项目解决方案文件 (支持 Rider/VS Code/VS 等)
Unnamedbot/                     # 项目主程序目录
└── ThirdParty/                 # 第三方依赖管理目录
    └── CubismSdkForNative-5-r.5/  # Live2D Cubism SDK 原生版 5-r.5
        ├── Core/               # 核心运行库 (Live2DCubismCore)
        ├── Framework/          # 框架层 (模型、动作、物理等逻辑)
        ├── Samples/            # 官方示例项目 (包含 D3D11, Metal, OpenGL 等完整 Demo)
        ├── LICENSE.md          # SDK 整体许可证
        └── README.md           # SDK 使用文档
```

## 环境配置与构建

项目构建主要依赖 CMake。请根据目标开发平台（Windows, Linux, Android 等）准备相应的 C/C++ 编译环境。

### 必要依赖

1.  **CMake**: 建议版本 3.15 或更高。
2.  **C/C++ 工具链**: 
    *   Windows: Visual Studio (MSVC) 或 MinGW-w64
    *   macOS/iOS: Xcode (Clang)
    *   Android: Android NDK
    *   Linux: GCC 或 Clang
3.  **图形 API SDK** (根据需要): 
    *   Windows: DirectX SDK
    *   通用: Vulkan SDK

### 构建说明

1.  **配置**: 使用 CMake 生成对应平台的构建文件。
2.  **编译**: 标准编译流程。
3.  **资源**: SDK 示例中通常包含 `Resources` 目录用于存放模型文件（`.moc3`, `.json` 等），请确保运行时这些资源路径正确。

详细的各平台编译指南和第三方库集成说明，请参阅：
`Unnamedbot/ThirdParty/CubismSdkForNative-5-r.5/README.md`

## 许可证 (License)

本项目集成了第三方软件，请特别注意以下许可协议：

*   **Live2D Cubism SDK**: 本项目使用的 Live2D SDK 遵循 **Live2D Proprietary Software License**。这意味着 SDK 本身是专有的，在商业使用或分发时需要遵守 Live2D 公司的许可协议。请查阅 `Unnamedbot/ThirdParty/CubismSdkForNative-5-r.5/LICENSE.md` 以及 `Core/LICENSE.md` 获取更多详情。
*   **项目源码**: `Unnamedbot/` 目录下的源码许可协议请以项目根目录下的 LICENSE 文件为准（如有），或咨询作者。

**警告**: 在商业产品中使用 Live2D 模型（.moc3 文件）通常需要开发者自行向 Live2D 公司购买授权。请确保合规使用。