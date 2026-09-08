# Unnamedbot

## Project Introduction

This project is a native application project based on the **Live2D Cubism SDK**, aiming to utilize Live2D core technology for the rendering and interaction of dynamic models (such as Virtual Host/Vtuber avatars).

### Main Technology Stack

*   **Live2D Cubism SDK**: Version `5-r.5`, provides core rendering capabilities.
*   **Multi-Platform Support**: Based on the SDK architecture, supports mainstream operating systems including Windows, macOS, Linux, Android, iOS, HarmonyOS, etc.
*   **Multiple Rendering Backends**: Integrates industry-leading graphics interfaces, including **OpenGL / OpenGL ES**, **DirectX 9 / 11**, **Metal**, and **Vulkan**.
*   **Build Tools**: Uses **CMake** for unified cross-platform project management.

## Directory Structure

```text
Unnamedbot.slnx                 # Project solution file (supports Rider/VS Code/VS, etc.)
Unnamedbot/                     # Project main program directory
└── ThirdParty/                 # Third-party dependency management directory
    └── CubismSdkForNative-5-r.5/  # Live2D Cubism SDK Native Version 5-r.5
        ├── Core/               # Core Runtime Library (Live2DCubismCore)
        ├── Framework/          # Framework Layer (Model, Motion, Physics logic, etc.)
        ├── Samples/            # Official Sample Projects (Includes complete Demos for D3D11, Metal, OpenGL, etc.)
        ├── LICENSE.md          # SDK Overall License
        └── README.md           # SDK Usage Documentation
```

## Environment Configuration & Building

Project building primarily relies on CMake. Please prepare the corresponding C/C++ compilation environment according to the target development platform (Windows, Linux, Android, etc.).

### Essential Dependencies

1.  **CMake**: Recommended version 3.15 or higher.
2.  **C/C++ Toolchain**: 
    *   Windows: Visual Studio (MSVC) or MinGW-w64
    *   macOS/iOS: Xcode (Clang)
    *   Android: Android NDK
    *   Linux: GCC or Clang
3.  **Graphics API SDK** (As needed): 
    *   Windows: DirectX SDK
    *   General: Vulkan SDK

### Build Instructions

1.  **Configure**: Use CMake to generate build files for the corresponding platform.
2.  **Compile**: Standard compilation process.
3.  **Resources**: SDK samples usually contain a `Resources` directory for storing model files (`.moc3`, `.json`, etc.). Please ensure these resource paths are correct during runtime.

For detailed compilation guides for each platform and third-party library integration instructions, please refer to:
`Unnamedbot/ThirdParty/CubismSdkForNative-5-r.5/README.md`

## License

This project integrates third-party software. Please pay special attention to the following license agreements:

*   **Live2D Cubism SDK**: The Live2D SDK used in this project follows the **Live2D Proprietary Software License**. This means the SDK itself is proprietary, and its commercial use or distribution must comply with the licensing agreement of Live2D Inc. Please refer to `Unnamedbot/ThirdParty/CubismSdkForNative-5-r.5/LICENSE.md` and `Core/LICENSE.md` for more details.
*   **Project Source Code**: The licensing agreement for the source code under `Unnamedbot/` is subject to the LICENSE file in the project root directory (if available), or consult the author.

**Warning**: Using Live2D models (`.moc3` files) in commercial products usually requires developers to purchase licensing directly from Live2D Inc. Please ensure compliance usage.