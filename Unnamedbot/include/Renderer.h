#pragma once
 
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <vector>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
// 桌宠窗口需要逐像素透明：交换链经 DirectComposition 呈现，纹理用 WIC 解码。
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace UnnamedBot {

    // 渲染器类 - 管理DX11设备、交换链、渲染目标
    class Renderer {
    public:
        // 交换链后台缓冲数量；Live2D D3D11 渲染器要求在加载模型前
        // 通过 SetConstantSettings 传入与交换链一致的缓冲数量及设备。
        static constexpr UINT BACK_BUFFER_COUNT = 2;

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

        // 设置窗口整体不透明度（0.0 ~ 1.0）。
        // 窗口使用 DirectComposition 呈现，分层窗口的 LWA_ALPHA 不再适用，
        // 透明度改由合成视觉（visual）实现，因此该值作用于整帧而非单个像素。
        void SetWindowOpacity(float opacity);

        // 获取当前FPS
        float GetCurrentFPS() const { return currentFPS_; }

        // 获取显存使用情况（MB）
        float GetVideoMemoryUsage() const;

        // 创建纹理（从文件或内存）
        ComPtr<ID3D11ShaderResourceView> CreateTextureFromFile(const std::wstring& filePath);
        ComPtr<ID3D11ShaderResourceView> CreateTextureFromMemory(const void* data, size_t size);

        // 从文件创建纹理视图（不依赖 Renderer 实例，供 Model 绑定 Live2D 贴图使用）
        static ComPtr<ID3D11ShaderResourceView> CreateTexture(ID3D11Device* device, const std::wstring& filePath);

        // 创建动态顶点/索引缓冲区
        ComPtr<ID3D11Buffer> CreateVertexBuffer(const void* data, size_t size, bool dynamic = true);
        ComPtr<ID3D11Buffer> CreateIndexBuffer(const void* data, size_t size, bool dynamic = true);

        // 创建常量缓冲区
        ComPtr<ID3D11Buffer> CreateConstantBuffer(size_t size);

        // 编译Shader
        ComPtr<ID3D11VertexShader> CreateVertexShader(const std::string& shaderSource, const std::string& entryPoint = "main");
        ComPtr<ID3D11PixelShader> CreatePixelShader(const std::string& shaderSource, const std::string& entryPoint = "main");

        // ---- 命中掩码 ----
        // 桌宠窗口要求「碰撞框只覆盖模型的非透明部分」。与其在 CPU 侧重现
        // Live2D 的顶点变换，不如直接把后台缓冲的 alpha 读回来：结果与画面上
        // 看到的像素完全一致，且与投影矩阵、模型缩放无关。
        //
        // CaptureHitMask 必须在渲染完一帧、Present 之前、在渲染线程上调用；
        // HitTestOpaque 可在任意线程调用（内部加锁读快照）。
        bool CaptureHitMask();
        bool HitTestOpaque(int x, int y) const;
        void InvalidateHitMask();

        // ---- 快速模式圆点 ----
        // 直接绘制到当前后台缓冲（调用前应先 BeginFrame）。坐标为像素，
        // 原点在左上角；输出预乘 alpha，与 Live2D 的输出保持一致。
        bool DrawDot(float centerX, float centerY, float radius, const DirectX::XMFLOAT4& color);

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

        // 创建圆点绘制所需的着色器与混合状态（首次绘制时惰性创建）
        bool EnsureDotResources();

        // 计算FPS
        void UpdateFPS();

        // 检查显存
        void CheckVideoMemory();

        // DX11设备
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGISwapChain> swapChain_;

        // DirectComposition 对象：合成目标、视觉、不透明度效果与设备。
        // 不透明度用 IDCompositionEffectGroup 施加：Win8 起即存在，
        // 而 CreateVisual 在部分系统上不返回 IDCompositionVisual3。
        ComPtr<IDCompositionDevice> dcompDevice_;
        ComPtr<IDCompositionTarget> dcompTarget_;
        ComPtr<IDCompositionVisual> dcompVisual_;
        ComPtr<IDCompositionEffectGroup> dcompEffectGroup_;
        ComPtr<ID3D11RenderTargetView> renderTargetView_;
        ComPtr<ID3D11DepthStencilView> depthStencilView_;
        ComPtr<ID3D11Texture2D> depthStencilBuffer_;
        ComPtr<ID3D11RasterizerState> rasterizerState_;
        ComPtr<ID3D11BlendState> blendState_;
        ComPtr<ID3D11SamplerState> samplerState_;

        // 后台缓冲纹理：命中掩码直接从它拷贝，避免每帧重新 GetBuffer
        ComPtr<ID3D11Texture2D> backBuffer_;

        // 命中掩码：每像素 1 字节 alpha
        ComPtr<ID3D11Texture2D> hitMaskStaging_;
        std::vector<unsigned char> hitMask_;
        int hitMaskWidth_ = 0;
        int hitMaskHeight_ = 0;
        bool hitMaskValid_ = false;
        mutable std::mutex hitMaskMutex_;

        // 暂存纹理尺寸（只在渲染线程上访问）
        int stagingWidth_ = 0;
        int stagingHeight_ = 0;

        // 圆点绘制资源
        ComPtr<ID3D11VertexShader> dotVertexShader_;
        ComPtr<ID3D11PixelShader> dotPixelShader_;
        ComPtr<ID3D11Buffer> dotConstantBuffer_;
        ComPtr<ID3D11BlendState> dotBlendState_;
        bool dotReady_ = false;

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