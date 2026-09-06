#include "../include/Renderer.h"
#include "../include/Logger.h"
#include <d3dcompiler.h>
#include <chrono>
#include <fstream>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace UnnamedBot {

    Renderer::Renderer() {
        QueryPerformanceFrequency(&perfFrequency_);
        frameInterval_ = 1.0 / 30.0;
    }

    Renderer::~Renderer() {
        Shutdown();
    }

    bool Renderer::Initialize(HWND hwnd, int width, int height, bool vsync) {
        if (!hwnd) {
            LOG_ERROR("Renderer", "Invalid window handle.");
            return false;
        }

        hwnd_ = hwnd;
        width_ = width;
        height_ = height;
        vsync_ = vsync;

        if (!CreateDevice(hwnd)) {
            LOG_ERROR("Renderer", "Failed to create D3D11 device.");
            return false;
        }

        if (!CreateSwapChain(hwnd)) {
            LOG_ERROR("Renderer", "Failed to create swap chain.");
            return false;
        }

        if (!CreateRenderTargetView()) {
            LOG_ERROR("Renderer", "Failed to create render target view.");
            return false;
        }

        if (!CreateDepthStencilView()) {
            LOG_ERROR("Renderer", "Failed to create depth stencil view.");
            return false;
        }

        CreateRasterizerState();
        CreateBlendState();
        CreateSamplerState();

        // 设置视口
        SetViewport(0, 0, width_, height_);

        LOG_INFO("Renderer", "D3D11 renderer initialized successfully. Width: " + std::to_string(width_) +
            ", Height: " + std::to_string(height_) + ", VSync: " + std::string(vsync ? "on" : "off"));
        return true;
    }

    void Renderer::Shutdown() {
        // 释放所有COM资源
        samplerState_.Reset();
        blendState_.Reset();
        rasterizerState_.Reset();
        depthStencilView_.Reset();
        depthStencilBuffer_.Reset();
        renderTargetView_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();

        hwnd_ = nullptr;
        LOG_INFO("Renderer", "Renderer shut down.");
    }

    void Renderer::Resize(int width, int height) {
        if (width <= 0 || height <= 0) {
            LOG_WARN("Renderer", "Invalid resize dimensions: " + std::to_string(width) + "x" + std::to_string(height));
            return;
        }

        // 释放与旧缓冲区相关的视图
        renderTargetView_.Reset();
        depthStencilView_.Reset();
        depthStencilBuffer_.Reset();

        if (swapChain_) {
            // 调整交换链缓冲区大小
            HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) {
                LOG_ERROR("Renderer", "Failed to resize swap chain buffers. Error: " + std::to_string(hr));
                return;
            }
        }

        width_ = width;
        height_ = height;
        aspectRatio_ = static_cast<float>(width) / static_cast<float>(height);

        // 重新创建渲染目标视图和深度模板视图
        if (!CreateRenderTargetView()) {
            LOG_ERROR("Renderer", "Failed to recreate render target view after resize.");
            return;
        }

        if (!CreateDepthStencilView()) {
            LOG_ERROR("Renderer", "Failed to recreate depth stencil view after resize.");
            return;
        }

        SetViewport(0, 0, width_, height_);
        LOG_DEBUG("Renderer", "Resized to: " + std::to_string(width_) + "x" + std::to_string(height_));
    }

    void Renderer::BeginFrame() {
        // 检查是否需要更新帧率控制
        UpdateFPS();

        // 清除渲染目标
        ClearRenderTarget(DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f));

        // 绑定渲染目标和深度模板视图
        context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), depthStencilView_.Get());

        // 应用光栅化状态、混合状态、采样状态
        context_->RSSetState(rasterizerState_.Get());
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    }

    void Renderer::EndFrame() {
        // 交换缓冲区
        HRESULT hr = swapChain_->Present(vsync_ ? 1 : 0, 0);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                LOG_ERROR("Renderer", "Device lost during Present. Need to recreate device.");
            }
            else {
                LOG_ERROR("Renderer", "Present failed. Error: " + std::to_string(hr));
            }
        }

        // 帧率控制（限制FPS）
        if (targetFPS_ > 0 && !vsync_) {
            double targetInterval = 1.0 / targetFPS_;
            double elapsed = GetTickCount64() / 1000.0 - lastFrameTime_;
            if (elapsed < targetInterval) {
                DWORD sleepTime = static_cast<DWORD>((targetInterval - elapsed) * 1000);
                if (sleepTime > 1) {
                    Sleep(sleepTime - 1);
                }
                // 等待剩余时间
                while (GetTickCount64() / 1000.0 - lastFrameTime_ < targetInterval) {
                    Sleep(0);
                }
            }
        }
    }

    void Renderer::ClearRenderTarget(const DirectX::XMFLOAT4& clearColor) {
        if (renderTargetView_) {
            context_->ClearRenderTargetView(renderTargetView_.Get(), &clearColor.x);
        }

        if (depthStencilView_) {
            context_->ClearDepthStencilView(depthStencilView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
    }

    void Renderer::SetViewport(int x, int y, int width, int height) {
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = static_cast<float>(x);
        viewport.TopLeftY = static_cast<float>(y);
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
    }

    void Renderer::SetTargetFPS(int fps) {
        if (fps > 0) {
            targetFPS_ = fps;
            frameInterval_ = 1.0 / fps;
        }
        else {
            targetFPS_ = 0;  // 不限制
            frameInterval_ = 0.0;
        }
        LOG_DEBUG("Renderer", "Target FPS set to: " + std::to_string(targetFPS_));
    }

    ComPtr<ID3D11ShaderResourceView> Renderer::CreateTextureFromFile(const std::wstring& filePath) {
        // 使用WIC加载纹理（简化版：使用DirectXTex或stb_image）
        // 这里使用一个简化的方法：加载DDS或使用STB
        // 实际项目中推荐使用DirectXTex库
        // 本实现仅作占位，实际需集成stb_image
        LOG_WARN("Renderer", "CreateTextureFromFile not fully implemented.");
        return nullptr;
    }

    ComPtr<ID3D11ShaderResourceView> Renderer::CreateTextureFromMemory(const void* data, size_t size) {
        LOG_WARN("Renderer", "CreateTextureFromMemory not fully implemented.");
        return nullptr;
    }

    ComPtr<ID3D11Buffer> Renderer::CreateVertexBuffer(const void* data, size_t size, bool dynamic) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        desc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA initData = {};
        if (data) {
            initData.pSysMem = data;
        }

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = device_->CreateBuffer(&desc, data ? &initData : nullptr, &buffer);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create vertex buffer. Error: " + std::to_string(hr));
            return nullptr;
        }
        return buffer;
    }

    ComPtr<ID3D11Buffer> Renderer::CreateIndexBuffer(const void* data, size_t size, bool dynamic) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        desc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA initData = {};
        if (data) {
            initData.pSysMem = data;
        }

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = device_->CreateBuffer(&desc, data ? &initData : nullptr, &buffer);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create index buffer. Error: " + std::to_string(hr));
            return nullptr;
        }
        return buffer;
    }

    ComPtr<ID3D11Buffer> Renderer::CreateConstantBuffer(size_t size) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = device_->CreateBuffer(&desc, nullptr, &buffer);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create constant buffer. Error: " + std::to_string(hr));
            return nullptr;
        }
        return buffer;
    }

    ComPtr<ID3D11VertexShader> Renderer::CreateVertexShader(const std::string& shaderSource, const std::string& entryPoint) {
        ComPtr<ID3DBlob> blob, errorBlob;
        HRESULT hr = D3DCompile(
            shaderSource.c_str(),
            shaderSource.size(),
            nullptr,
            nullptr,
            nullptr,
            entryPoint.c_str(),
            "vs_5_0",
            D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
            0,
            &blob,
            &errorBlob
        );

        if (FAILED(hr)) {
            std::string errorMsg = "Failed to compile vertex shader. ";
            if (errorBlob) {
                errorMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            LOG_ERROR("Renderer", errorMsg);
            return nullptr;
        }

        ComPtr<ID3D11VertexShader> shader;
        hr = device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create vertex shader. Error: " + std::to_string(hr));
            return nullptr;
        }
        return shader;
    }

    ComPtr<ID3D11PixelShader> Renderer::CreatePixelShader(const std::string& shaderSource, const std::string& entryPoint) {
        ComPtr<ID3DBlob> blob, errorBlob;
        HRESULT hr = D3DCompile(
            shaderSource.c_str(),
            shaderSource.size(),
            nullptr,
            nullptr,
            nullptr,
            entryPoint.c_str(),
            "ps_5_0",
            D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
            0,
            &blob,
            &errorBlob
        );

        if (FAILED(hr)) {
            std::string errorMsg = "Failed to compile pixel shader. ";
            if (errorBlob) {
                errorMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            LOG_ERROR("Renderer", errorMsg);
            return nullptr;
        }

        ComPtr<ID3D11PixelShader> shader;
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create pixel shader. Error: " + std::to_string(hr));
            return nullptr;
        }
        return shader;
    }

    float Renderer::GetVideoMemoryUsage() const {
        // 检查显存使用情况
        // 使用DXGI查询
        if (!device_) return 0.0f;

        // 简化实现：尝试获取适配器内存信息
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr)) {
            ComPtr<IDXGIAdapter> adapter;
            hr = dxgiDevice->GetAdapter(&adapter);
            if (SUCCEEDED(hr)) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = {};
                // 注意：此函数仅在Windows 10 20H2+支持
                // 可以尝试使用GetDesc获取总显存
                DXGI_ADAPTER_DESC desc;
                hr = adapter->GetDesc(&desc);
                if (SUCCEEDED(hr)) {
                    // 返回总显存（简化）
                    return static_cast<float>(desc.DedicatedVideoMemory) / (1024.0f * 1024.0f);
                }
            }
        }

        // 降级方案：返回0
        return 0.0f;
    }

    // 私有方法实现

    bool Renderer::CreateDevice(HWND hwnd) {
        UINT createDeviceFlags = 0;
#ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &device_,
            &selectedLevel,
            &context_
        );

        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create D3D11 device. Error: " + std::to_string(hr));
            return false;
        }

        LOG_INFO("Renderer", "D3D11 device created. Feature Level: " +
            std::to_string(static_cast<int>(selectedLevel) >> 16) + "." +
            std::to_string(static_cast<int>(selectedLevel) & 0xFFFF));
        return true;
    }

    bool Renderer::CreateSwapChain(HWND hwnd) {
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to get DXGI device. Error: " + std::to_string(hr));
            return false;
        }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to get DXGI adapter. Error: " + std::to_string(hr));
            return false;
        }

        ComPtr<IDXGIFactory> factory;
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to get DXGI factory. Error: " + std::to_string(hr));
            return false;
        }

        DXGI_SWAP_CHAIN_DESC scDesc = {};
        scDesc.BufferDesc.Width = width_;
        scDesc.BufferDesc.Height = height_;
        scDesc.BufferDesc.RefreshRate.Numerator = 60;
        scDesc.BufferDesc.RefreshRate.Denominator = 1;
        scDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        scDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        scDesc.SampleDesc.Count = 1;
        scDesc.SampleDesc.Quality = 0;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.OutputWindow = hwnd;
        scDesc.Windowed = TRUE;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        hr = factory->CreateSwapChain(device_.Get(), &scDesc, &swapChain_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create swap chain. Error: " + std::to_string(hr));
            return false;
        }

        return true;
    }

    bool Renderer::CreateRenderTargetView() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to get back buffer. Error: " + std::to_string(hr));
            return false;
        }

        hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create render target view. Error: " + std::to_string(hr));
            return false;
        }

        return true;
    }

    bool Renderer::CreateDepthStencilView() {
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = width_;
        depthDesc.Height = height_;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.SampleDesc.Quality = 0;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        depthDesc.CPUAccessFlags = 0;
        depthDesc.MiscFlags = 0;

        HRESULT hr = device_->CreateTexture2D(&depthDesc, nullptr, &depthStencilBuffer_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create depth stencil texture. Error: " + std::to_string(hr));
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = depthDesc.Format;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;

        hr = device_->CreateDepthStencilView(depthStencilBuffer_.Get(), &dsvDesc, &depthStencilView_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create depth stencil view. Error: " + std::to_string(hr));
            return false;
        }

        return true;
    }

    void Renderer::CreateRasterizerState() {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.FrontCounterClockwise = FALSE;
        desc.DepthBias = 0;
        desc.SlopeScaledDepthBias = 0.0f;
        desc.DepthBiasClamp = 0.0f;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = FALSE;
        desc.MultisampleEnable = TRUE;
        desc.AntialiasedLineEnable = TRUE;

        HRESULT hr = device_->CreateRasterizerState(&desc, &rasterizerState_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create rasterizer state. Error: " + std::to_string(hr));
        }
    }

    void Renderer::CreateBlendState() {
        D3D11_BLEND_DESC desc = {};
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        HRESULT hr = device_->CreateBlendState(&desc, &blendState_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create blend state. Error: " + std::to_string(hr));
        }
    }

    void Renderer::CreateSamplerState() {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        desc.BorderColor[0] = 0.0f;
        desc.BorderColor[1] = 0.0f;
        desc.BorderColor[2] = 0.0f;
        desc.BorderColor[3] = 0.0f;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;

        HRESULT hr = device_->CreateSamplerState(&desc, &samplerState_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create sampler state. Error: " + std::to_string(hr));
        }
    }

    void Renderer::UpdateFPS() {
        static double lastTime = 0.0;
        static int frameCount = 0;
        double currentTime = GetTickCount64() / 1000.0;

        frameCount++;

        if (currentTime - lastTime >= 1.0) {
            currentFPS_ = static_cast<float>(frameCount) / static_cast<float>(currentTime - lastTime);
            frameCount = 0;
            lastTime = currentTime;
        }
    }

    void Renderer::CheckVideoMemory() {
        DWORD64 currentTime = GetTickCount64();
        if (currentTime - lastMemoryCheckTime_ > 5000) { // 每5秒检查一次
            videoMemoryUsage_ = GetVideoMemoryUsage();
            lastMemoryCheckTime_ = currentTime;
        }
    }

} // namespace UnnamedBot