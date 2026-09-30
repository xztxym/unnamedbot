#include "../include/Renderer.h"
#include "../include/Logger.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace UnnamedBot {

    namespace {

        // 圆点绘制的常量缓冲，字段顺序与 HLSL 的 cbuffer DotConstants 逐项对应。
        // 显式排出 4 个 float 一组：HLSL 的常量缓冲按 16 字节打包，
        // 写错顺序不会编译报错，只会画出错误的图形。
        struct DotConstants {
            float center[2];
            float radius;
            float pad0;
            float size[2];
            float pad1;
            float pad2;
            DirectX::XMFLOAT4 color;
        };

    } // namespace

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

        // 合成交换链（CreateSwapChainForComposition）必须显式给出合法尺寸：
        // 0、负数或超大值都会让它返回 DXGI_ERROR_INVALID_CALL，导致渲染器
        // 初始化失败、进程在显示窗口前就退出。
        if (width < 1 || width > 16384 || height < 1 || height > 16384) {
            LOG_WARN("Renderer", "Invalid render size " + std::to_string(width) + "x" +
                std::to_string(height) + ", falling back to 400x400.");
            width = 400;
            height = 400;
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
        dcompVisual_.Reset();
        dcompTarget_.Reset();
        dcompDevice_.Reset();
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
        // 命中掩码持有的后台缓冲引用必须先放开：
        // DXGI 要求在 ResizeBuffers 之前释放所有对交换链缓冲的引用，
        // 否则这里会返回 DXGI_ERROR_INVALID_CALL，窗口一改变大小就再也不出画面。
        backBuffer_.Reset();
        InvalidateHitMask();

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
        return CreateTexture(device_.Get(), filePath);
    }

    ComPtr<ID3D11ShaderResourceView> Renderer::CreateTexture(ID3D11Device* device, const std::wstring& filePath) {
        if (!device || filePath.empty()) {
            return nullptr;
        }

        // 用 WIC 把 PNG/JPG 解码为 32bpp BGRA。
        // 必须保持直通（straight）alpha：Live2D 的 PixelNormal 着色器会自行
        // 把 rgb 乘以 alpha，若此处预先乘算会重复相乘，边缘会发黑。
        ComPtr<IWICImagingFactory> wicFactory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create WIC imaging factory. Error: " + std::to_string(hr));
            return nullptr;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        hr = wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to open texture file. Error: " + std::to_string(hr));
            return nullptr;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to decode texture frame. Error: " + std::to_string(hr));
            return nullptr;
        }

        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create WIC format converter. Error: " + std::to_string(hr));
            return nullptr;
        }

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to convert texture to BGRA. Error: " + std::to_string(hr));
            return nullptr;
        }

        UINT textureWidth = 0;
        UINT textureHeight = 0;
        hr = converter->GetSize(&textureWidth, &textureHeight);
        if (FAILED(hr) || textureWidth == 0 || textureHeight == 0) {
            LOG_ERROR("Renderer", "Texture has invalid size.");
            return nullptr;
        }

        const UINT stride = textureWidth * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * textureHeight);
        hr = converter->CopyPixels(nullptr, stride,
                                   static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to copy texture pixels. Error: " + std::to_string(hr));
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = textureWidth;
        textureDesc.Height = textureHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textureDesc.CPUAccessFlags = 0;
        textureDesc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA initialData = {};
        initialData.pSysMem = pixels.data();
        initialData.SysMemPitch = stride;

        ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D(&textureDesc, &initialData, &texture);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create texture. Error: " + std::to_string(hr));
            return nullptr;
        }

        ComPtr<ID3D11ShaderResourceView> textureView;
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, &textureView);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create texture view. Error: " + std::to_string(hr));
            return nullptr;
        }

        return textureView;
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

        // D3D_FEATURE_LEVEL 的高位字节编码为 0xMM00（如 0xB100 表示 11.1），
        // 主版本在高 4 位、次版本在次高 4 位，不能直接取高低 16 位。
        const unsigned level = static_cast<unsigned>(selectedLevel);
        LOG_INFO("Renderer", "D3D11 device created. Feature Level: " +
            std::to_string((level >> 12) & 0xF) + "." +
            std::to_string((level >> 8) & 0xF));
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

        ComPtr<IDXGIFactory2> factory;
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to get DXGI factory. Error: " + std::to_string(hr));
            return false;
        }

        // 桌宠需要逐像素透明：普通 HWND 交换链的 alpha 会被系统忽略（只能得到黑底），
        // 只有以合成（composition）方式创建的交换链才会把 backbuffer 的 alpha 交给
        // DWM 合成，从而实现「模型直接浮在桌面上」。
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = width_;
        scDesc.Height = height_;
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.SampleDesc.Quality = 0;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = BACK_BUFFER_COUNT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory->CreateSwapChainForComposition(device_.Get(), &scDesc, nullptr, &swapChain1);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create composition swap chain. Error: " + std::to_string(hr));
            return false;
        }
        swapChain_ = swapChain1;

        // 合成交换链不会自己上屏，必须挂到窗口的合成视觉上，否则窗口一片空白。
        hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create DirectComposition device. Error: " + std::to_string(hr));
            return false;
        }

        hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create DirectComposition target. Error: " + std::to_string(hr));
            return false;
        }

        ComPtr<IDCompositionVisual> visual;
        hr = dcompDevice_->CreateVisual(&visual);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create DirectComposition visual. Error: " + std::to_string(hr));
            return false;
        }

        // 整窗不透明度不用 IDCompositionVisual3::SetOpacity：
        // 实测 CreateVisual 返回的视觉在部分系统上无法 QI 到 Visual3（E_NOINTERFACE）。
        // 改用自 Win8 起就有的效果组，并且它失败也不影响渲染。
        hr = dcompDevice_->CreateEffectGroup(&dcompEffectGroup_);
        if (FAILED(hr)) {
            LOG_WARN("Renderer", "Failed to create DirectComposition effect group. Error: " + std::to_string(hr));
            dcompEffectGroup_.Reset();
        }

        dcompVisual_ = visual;
        if (dcompEffectGroup_) {
            dcompVisual_->SetEffect(dcompEffectGroup_.Get());
        }
        dcompVisual_->SetContent(swapChain_.Get());
        dcompTarget_->SetRoot(dcompVisual_.Get());
        dcompDevice_->Commit();

        return true;
    }

    void Renderer::SetWindowOpacity(float opacity) {
        if (!dcompEffectGroup_ || !dcompDevice_) {
            return;
        }

        // SetOpacity 设置的是绝对不透明度，重复调用不会累乘，可安全用于配置重载。
        dcompEffectGroup_->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
        dcompDevice_->Commit();
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

        // 顺手留住后台缓冲：命中掩码每帧都要从它拷贝，不必反复 GetBuffer。
        // 注意 Resize 里必须在 ResizeBuffers 之前释放，否则交换链无法重建缓冲。
        backBuffer_ = backBuffer;
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

    bool Renderer::CaptureHitMask() {
        if (!swapChain_ || !context_ || !device_) {
            return false;
        }

        // Resize 之后 backBuffer_ 会被释放，这里补一次。
        if (!backBuffer_) {
            HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_));
            if (FAILED(hr)) {
                return false;
            }
        }

        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer_->GetDesc(&desc);

        // 暂存纹理按尺寸缓存：窗口尺寸变了才重建。
        if (!hitMaskStaging_ || stagingWidth_ != static_cast<int>(desc.Width)
            || stagingHeight_ != static_cast<int>(desc.Height)) {
            hitMaskStaging_.Reset();
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            HRESULT hr = device_->CreateTexture2D(&stagingDesc, nullptr, &hitMaskStaging_);
            if (FAILED(hr)) {
                LOG_WARN("Renderer", "Failed to create hit mask staging texture. Error: " + std::to_string(hr));
                return false;
            }
            stagingWidth_ = static_cast<int>(desc.Width);
            stagingHeight_ = static_cast<int>(desc.Height);
        }

        // 作为渲染目标绑定时 CopyResource 会失败，先解绑。
        ID3D11RenderTargetView* noTargets[1] = { nullptr };
        context_->OMSetRenderTargets(1, noTargets, nullptr);
        context_->CopyResource(hitMaskStaging_.Get(), backBuffer_.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context_->Map(hitMaskStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            LOG_WARN("Renderer", "Failed to map hit mask staging texture. Error: " + std::to_string(hr));
            return false;
        }

        const int width = static_cast<int>(desc.Width);
        const int height = static_cast<int>(desc.Height);
        // BGRA/RGBA 每像素 4 字节，alpha 在第 4 个字节；X8 格式没有 alpha，
        // 视为整块不透明（此时碰撞框等于窗口矩形，退回老行为）。
        const bool hasAlpha = desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM
            && desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

        {
            std::lock_guard<std::mutex> lock(hitMaskMutex_);
            hitMask_.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
            const unsigned char* base = static_cast<const unsigned char*>(mapped.pData);
            for (int y = 0; y < height; ++y) {
                const unsigned char* row = base + static_cast<size_t>(y) * mapped.RowPitch;
                unsigned char* destination = hitMask_.data() + static_cast<size_t>(y) * width;
                if (!hasAlpha) {
                    memset(destination, 0xFF, static_cast<size_t>(width));
                    continue;
                }
                for (int x = 0; x < width; ++x) {
                    destination[x] = row[static_cast<size_t>(x) * 4 + 3];
                }
            }
            hitMaskWidth_ = width;
            hitMaskHeight_ = height;
            hitMaskValid_ = true;
        }

        context_->Unmap(hitMaskStaging_.Get(), 0);
        return true;
    }

    bool Renderer::HitTestOpaque(int x, int y) const {
        std::lock_guard<std::mutex> lock(hitMaskMutex_);
        if (!hitMaskValid_ || x < 0 || y < 0 || x >= hitMaskWidth_ || y >= hitMaskHeight_) {
            return false;
        }
        // 阈值取 8 而不是 0：模型边缘的抗锯齿像素 alpha 很低，肉眼已经算「模型外面」，
        // 用 0 会让碰撞区域比看到的轮廓大出一圈。
        return hitMask_[static_cast<size_t>(y) * hitMaskWidth_ + x] > 8;
    }

    void Renderer::InvalidateHitMask() {
        std::lock_guard<std::mutex> lock(hitMaskMutex_);
        hitMaskValid_ = false;
    }

    bool Renderer::EnsureDotResources() {
        if (dotReady_) {
            return true;
        }
        if (!device_) {
            return false;
        }

        // 一个顶点着色器 + 一个像素着色器搞定圆形：顶点着色器用 SV_VertexID 直接生成
        // 覆盖全屏的大三角形，像素着色器按到圆心的距离算覆盖率。
        // 不用顶点缓冲、不用几何体，圆点就永远不需要额外的资源管理。
        static const char* kDotShaderSource = R"(
cbuffer DotConstants : register(b0)
{
    float2 center;
    float  radius;
    float  pad0;
    float2 size;
    float  pad1;
    float  pad2;
    float4 color;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // 全屏三角形：uv 取 (0,0)(2,0)(0,2)，映射后覆盖整个裁剪空间
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float distanceToCenter = distance(input.position.xy, center);
    // 1 像素的软化边缘：圆点在 100% 缩放下也不会看到阶梯
    float coverage = saturate(radius - distanceToCenter + 0.5f);
    if (coverage <= 0.0f) {
        discard;
    }
    float alpha = color.a * coverage;
    // 交换链是 PREMULTIPLIED，输出必须预乘
    return float4(color.rgb * alpha, alpha);
}
)";

        dotVertexShader_ = CreateVertexShader(kDotShaderSource, "VSMain");
        dotPixelShader_ = CreatePixelShader(kDotShaderSource, "PSMain");
        if (!dotVertexShader_ || !dotPixelShader_) {
            LOG_ERROR("Renderer", "Failed to create dot shaders.");
            dotVertexShader_.Reset();
            dotPixelShader_.Reset();
            return false;
        }

        dotConstantBuffer_ = CreateConstantBuffer(sizeof(DotConstants));
        if (!dotConstantBuffer_) {
            LOG_ERROR("Renderer", "Failed to create dot constant buffer.");
            return false;
        }

        // 圆点是画到「已经清成透明」的后台缓冲上，所以用预乘混合：
        // 目标是被覆盖的像素，而不是做透明混合的半透明层。
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        const HRESULT hr = device_->CreateBlendState(&blendDesc, &dotBlendState_);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer", "Failed to create dot blend state. Error: " + std::to_string(hr));
            return false;
        }

        dotReady_ = true;
        return true;
    }

    bool Renderer::DrawDot(float centerX, float centerY, float radius, const DirectX::XMFLOAT4& color) {
        if (!context_ || radius <= 0.0f) {
            return false;
        }
        if (!EnsureDotResources()) {
            return false;
        }

        DotConstants constants = {};
        constants.center[0] = centerX;
        constants.center[1] = centerY;
        constants.radius = radius;
        constants.size[0] = static_cast<float>(width_);
        constants.size[1] = static_cast<float>(height_);
        constants.color = color;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(dotConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(dotConstantBuffer_.Get(), 0);

        // 圆点可以在任意窗口尺寸下绘制（快速模式下窗口仍然保持配置尺寸），
        // 因此每次绘制都把视口设回整窗，避免沿用上一次的残留状态。
        SetViewport(0, 0, width_, height_);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(dotVertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(dotPixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, dotConstantBuffer_.GetAddressOf());
        context_->OMSetBlendState(dotBlendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->Draw(3, 0);

        // 还原为 Live2D 使用的混合状态：同一帧里两者可能先后绘制
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        return true;
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