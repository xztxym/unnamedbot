#pragma once
 
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace UnnamedBot {

    // 渲染器类 - 管理DX11设备、交换链、渲染目标
    class Renderer {
    public:
        Renderer();
        ~Renderer();

        // 初始化渲染器
        bool Initialize(HWND hwnd, int width, int height, bool vsync = false);

        // 销毁渲染器
        void Shutdown();

        // 重置渲染目标大小（窗口大小变化时）
        void Resize(int width, int height);

        // 开始渲染帧
        void BeginFrame();

        // 结束渲染帧并交换缓冲
        void EndFrame();

        // 清除渲染目标（颜色）
        void ClearRenderTarget(const DirectX::XMFLOAT4& clearColor = { 0.0f, 0.0f, 0.0f, 0.0f });

        // 获取设备上下文
        ID3D11Device* GetDevice() const { return device_.Get(); }
        ID3D11DeviceContext* GetContext() const { return context_.Get(); }

        // 获取渲染目标视图
        ID3D11RenderTargetView* GetRenderTargetView() const { return renderTargetView_.Get(); }

        // 获取深度模板视图
        ID3D11DepthStencilView* GetDepthStencilView() const { return depthStencilView_.Get(); }

        // 获取交换链
        IDXGISwapChain* GetSwapChain() const { return swapChain_.Get(); }

        // 获取窗口大小
        void GetSize(int& width, int& height) const { width = width_; height = height_; }

        // 设置视口
        void SetViewport(int x, int y, int width, int height);

        // 设置VSync
        void SetVSync(bool enabled) { vsync_ = enabled; }

        // 获取VSync状态
        bool GetVSync() const { return vsync_; }

        // 锁定帧率（用于后台降帧）
        void SetTargetFPS(int fps);
        int GetTargetFPS() const { return targetFPS_; }

        // 获取当前FPS
        float GetCurrentFPS() const { return currentFPS_; }

        // 获取显存使用情况（MB）
        float GetVideoMemoryUsage() const;

        // 创建纹理（从文件或内存）
        ComPtr<ID3D11ShaderResourceView> CreateTextureFromFile(const std::wstring& filePath);
        ComPtr<ID3D11ShaderResourceView> CreateTextureFromMemory(const void* data, size_t size);

        // 创建动态顶点/索引缓冲区
        ComPtr<ID3D11Buffer> CreateVertexBuffer(const void* data, size_t size, bool dynamic = true);
        ComPtr<ID3D11Buffer> CreateIndexBuffer(const void* data, size_t size, bool dynamic = true);

        // 创建常量缓冲区
        ComPtr<ID3D11Buffer> CreateConstantBuffer(size_t size);

        // 编译Shader
        ComPtr<ID3D11VertexShader> CreateVertexShader(const std::string& shaderSource, const std::string& entryPoint = "main");
        ComPtr<ID3D11PixelShader> CreatePixelShader(const std::string& shaderSource, const std::string& entryPoint = "main");

    private:
        // 初始化设备
        bool CreateDevice(HWND hwnd);

        // 创建交换链
        bool CreateSwapChain(HWND hwnd);

        // 创建渲染目标视图
        bool CreateRenderTargetView();

        // 创建深度模板视图
        bool CreateDepthStencilView();

        // 创建光栅化状态
        void CreateRasterizerState();

        // 创建混合状态
        void CreateBlendState();

        // 创建采样状态
        void CreateSamplerState();

        // 计算FPS
        void UpdateFPS();

        // 检查显存
        void CheckVideoMemory();

        // DX11设备
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGISwapChain> swapChain_;
        ComPtr<ID3D11RenderTargetView> renderTargetView_;
        ComPtr<ID3D11DepthStencilView> depthStencilView_;
        ComPtr<ID3D11Texture2D> depthStencilBuffer_;
        ComPtr<ID3D11RasterizerState> rasterizerState_;
        ComPtr<ID3D11BlendState> blendState_;
        ComPtr<ID3D11SamplerState> samplerState_;

        // 窗口和渲染尺寸
        HWND hwnd_ = nullptr;
        int width_ = 0;
        int height_ = 0;
        float aspectRatio_ = 1.0f;

        // 帧率控制
        int targetFPS_ = 30;      // 默认30fps
        float currentFPS_ = 0.0f;
        double lastFPSTime_ = 0.0;
        int frameCount_ = 0;
        double lastFrameTime_ = 0.0;
        double frameInterval_ = 1.0 / 30.0;

        // 同步
        bool vsync_ = false;

        // 显存监控
        float videoMemoryUsage_ = 0.0f;
        DWORD64 lastMemoryCheckTime_ = 0;

        // 性能计数器频率
        LARGE_INTEGER perfFrequency_ = {};
    };

} // namespace UnnamedBot