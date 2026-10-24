#include "../include/Live2DManager.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Renderer.h"
#include "../include/Utils.h"
#include <CubismFramework.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <ICubismAllocator.hpp>
#include <cstdlib>
#include <malloc.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

namespace {

    // Cubism 框架在 StartUp 时要求提供一个非空的内存分配器实现。
    class CubismAllocator : public Csm::ICubismAllocator {
    public:
        void* Allocate(const Csm::csmSizeType size) override {
            return std::malloc(size);
        }

        void Deallocate(void* memory) override {
            std::free(memory);
        }

        void* AllocateAligned(const Csm::csmSizeType size, const Csm::csmUint32 alignment) override {
            return _aligned_malloc(size, alignment);
        }

        void DeallocateAligned(void* alignedMemory) override {
            _aligned_free(alignedMemory);
        }
    };

    // D3D11 渲染器创建着色器时，会通过框架的文件加载回调读取相对路径
    // "FrameworkShaders/CubismEffect.fx"。该路径先按当前工作目录解析，
    // 再按可执行文件所在目录解析（VS 调试时的工作目录默认是工程目录，而非 exe 目录）。
    Csm::csmByte* LoadFrameworkFile(const std::string filePath, Csm::csmSizeInt* outSize) {
        if (outSize == nullptr) {
            return nullptr;
        }
        *outSize = 0;

        std::vector<std::filesystem::path> candidates;
        candidates.push_back(std::filesystem::path(Utils::Utf8ToWide(filePath)));

        wchar_t exePath[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0) {
            candidates.push_back(std::filesystem::path(exePath).parent_path() / Utils::Utf8ToWide(filePath));
        }

        for (const auto& candidate : candidates) {
            std::ifstream file(candidate, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                continue;
            }

            const std::streamoff size = file.tellg();
            if (size <= 0) {
                continue;
            }

            // 框架要求该缓冲区由其自身分配器分配，并由 ReleaseBytesFunction 释放。
            auto* bytes = static_cast<Csm::csmByte*>(
                Csm::CubismFramework::Allocate(static_cast<Csm::csmSizeType>(size)));
            if (bytes == nullptr) {
                continue;
            }

            file.seekg(0, std::ios::beg);
            if (file.read(reinterpret_cast<char*>(bytes), size)) {
                *outSize = static_cast<Csm::csmSizeInt>(size);
                return bytes;
            }

            Csm::CubismFramework::Deallocate(bytes);
        }

        LOG_WARN("Live2DManager", "Failed to load Live2D framework file: " + filePath);
        return nullptr;
    }

    void ReleaseFrameworkFile(Csm::csmByte* byteData) {
        if (byteData != nullptr) {
            Csm::CubismFramework::Deallocate(byteData);
        }
    }

} // namespace

    Live2DManager& Live2DManager::Instance() {
        static Live2DManager instance;
        return instance;
    }

    bool Live2DManager::Initialize(Renderer* renderer) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("Live2DManager", "Live2D framework already initialized.");
            return true;
        }

        // 启动并初始化 Live2D 框架。
        // R5 版 API 需要先通过 StartUp 传入内存分配器与配置（日志回调/日志级别），
        // 之后才能调用 Initialize()。StartUp 只保存 Option 指针，因此二者必须是静态存储。
        static CubismAllocator s_allocator;
        static Csm::CubismFramework::Option s_option;
        s_option.LogFunction = [](const char* message) {
            std::string msg = message ? message : "";
            LOG_DEBUG("Live2D", msg);
        };
        s_option.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
        // 不设置文件加载回调时，D3D11 渲染器读不到着色器源码，模型不会被绘制。
        s_option.LoadFileFunction = LoadFrameworkFile;
        s_option.ReleaseBytesFunction = ReleaseFrameworkFile;

        Csm::CubismFramework::StartUp(&s_allocator, &s_option);
        Csm::CubismFramework::Initialize();

        // D3D11 渲染器创建实例前必须知道使用哪个设备和缓冲数量，
        // 否则内部初始化会因设备为空而直接失败（模型不会被绘制）。
        if (renderer == nullptr || renderer->GetDevice() == nullptr) {
            LOG_ERROR("Live2DManager", "Valid D3D11 renderer is required to initialize Live2D.");
            return false;
        }
        Csm::Rendering::CubismRenderer_D3D11::SetConstantSettings(
            Renderer::BACK_BUFFER_COUNT, renderer->GetDevice());

        // 记录设备：之后加载模型时需要用它创建贴图纹理
        device_ = renderer->GetDevice();

        initialized_ = true;
        LOG_INFO("Live2DManager", "Live2D framework initialized.");

        // 扫描模型目录
        // 模型目录同样是「工作目录优先、否则 exe 目录」：装到别处后
        // ./models 往往不存在，而 exe 旁边就是安装时放好的 models。
        {
            // 统一用 ResolveAppRelative：工作目录里有就用工作目录，否则回退到 exe 目录。
            // （以前只在「工作目录下不存在」时才回退，且用的是裸相对路径，
            //   从快捷方式启动时会扫不到安装目录里的 models。）
            const std::filesystem::path resolved = Utils::ResolveAppRelative(L"models");
            std::error_code ec;
            if (!resolved.empty() && std::filesystem::exists(resolved, ec)) {
                modelDirectory_ = resolved.wstring();
                LOG_INFO("Live2DManager", "Model directory resolved to: " +
                    Utils::PathToUtf8(modelDirectory_));
            }
        }
        ScanModelDirectory();

        // 恢复模型状态
        RestoreModelState();

        return true;
    }

    void Live2DManager::Shutdown() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // 卸载所有模型
        for (auto& pair : models_) {
            ReleaseModelResources(pair.first);
        }
        models_.clear();
        currentModel_ = nullptr;
        currentModelId_.clear();

        // 关闭Live2D框架
        Csm::CubismFramework::Dispose();
        initialized_ = false;

        LOG_INFO("Live2DManager", "Live2D framework shut down.");
    }

    bool Live2DManager::LoadModel(const std::string& modelId, const std::wstring& modelPath) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("Live2DManager", "Live2D framework not initialized.");
            return false;
        }

        if (IsModelLoaded(modelId)) {
            LOG_WARN("Live2DManager", "Model already loaded: " + modelId);
            return true;
        }

        LOG_INFO("Live2DManager", "Loading model: " + modelId + " from " + Utils::PathToUtf8(modelPath));

        // 创建模型实例
        auto model = std::make_shared<Model>();
        if (!model->Load(modelPath, device_)) {
            LOG_ERROR("Live2DManager", "Failed to load model: " + modelId);
            return false;
        }

        models_[modelId] = model;
        LOG_INFO("Live2DManager", "Model loaded successfully: " + modelId);

        // 如果当前没有模型，自动切换
        if (!currentModel_) {
            SwitchModel(modelId);
        }

        return true;
    }

    void Live2DManager::UnloadModel(const std::string& modelId) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        auto it = models_.find(modelId);
        if (it == models_.end()) {
            LOG_WARN("Live2DManager", "Model not found: " + modelId);
            return;
        }

        // 如果正在使用该模型，切换
        if (currentModelId_ == modelId) {
            ReleaseModelResources(modelId);
            currentModel_ = nullptr;
            currentModelId_.clear();
        }

        models_.erase(it);
        LOG_INFO("Live2DManager", "Model unloaded: " + modelId);
    }

    bool Live2DManager::SwitchModel(const std::string& modelId) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("Live2DManager", "Live2D framework not initialized.");
            return false;
        }

        auto it = models_.find(modelId);
        if (it == models_.end()) {
            LOG_ERROR("Live2DManager", "Model not found: " + modelId);
            return false;
        }

        // 重复切到「就是当前这个」模型时必须直接返回：下面那段「先释放再切回」
        // 会把刚建好的渲染器和贴图一起销毁（Model::Release），之后 Render() 取到
        // 空渲染器就直接 return，模型再也不会上屏。启动路径恰好会切两次
        // （LoadModel 里的自动切换 + SwitchModelById 的显式切换），
        // 结果就是桌宠窗口在、模型却一片空白。
        if (currentModel_ && currentModelId_ == modelId) {
            const float sameScale = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, 1.0);
            const float sameOpacity = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, 1.0);
            it->second->SetScale(sameScale);
            it->second->SetOpacity(sameOpacity);
            it->second->SetPosition(
                ConfigManager::Instance().GetDouble("model.position_x", 0.0),
                ConfigManager::Instance().GetDouble("model.position_y", 0.0));
            LOG_INFO("Live2DManager", "Model already current, skipped reload: " + modelId);
            return true;
        }

        // 释放当前模型资源，并把它从缓存里摘掉：Model::Release 之后它已经没有
        // 渲染器/贴图了，留在 models_ 里会被下一次切换当成「已加载」直接切过去，
        // 得到的又是一个画不出东西的空模型。
        if (currentModel_) {
            const std::string previousId = currentModelId_;
            currentModel_ = nullptr;
            UnloadModel(previousId);

            it = models_.find(modelId);
            if (it == models_.end()) {
                LOG_ERROR("Live2DManager", "Model unloading failed: " + modelId);
                return false;
            }
        }

        // 切换到新模型
        currentModel_ = it->second;
        currentModelId_ = modelId;

        // 应用配置
        float scale = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, 1.0);
        float opacity = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, 1.0);
        currentModel_->SetScale(scale);
        currentModel_->SetOpacity(opacity);

        // 恢复位置
        float x = ConfigManager::Instance().GetDouble("model.position_x", 0.0);
        float y = ConfigManager::Instance().GetDouble("model.position_y", 0.0);
        currentModel_->SetPosition(x, y);

        LOG_INFO("Live2DManager", "Switched to model: " + modelId);
        return true;
    }

    std::vector<std::string> Live2DManager::GetLoadedModelIds() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (const auto& pair : models_) {
            ids.push_back(pair.first);
        }
        return ids;
    }

    bool Live2DManager::IsModelLoaded(const std::string& modelId) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return models_.find(modelId) != models_.end();
    }

    void Live2DManager::Update(float deltaTime) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->Update(deltaTime);
        }
    }

    void Live2DManager::Render(Renderer* renderer) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!renderer || !currentModel_) {
            return;
        }

        // 获取DX11设备上下文
        auto context = renderer->GetContext();
        if (!context) {
            return;
        }

        // 获取渲染器
        auto cubismRenderer = currentModel_->GetRenderer();
        if (!cubismRenderer) {
            return;
        }

        // 设置渲染目标
        auto renderTarget = renderer->GetRenderTargetView();
        if (!renderTarget) {
            return;
        }

        // Cubism 的 D3D11 渲染器要求每帧先 StartFrame 绑定设备上下文，
        // 绘制结束后 EndFrame 释放；否则 DrawModel 会解引用空上下文而崩溃。
        int width = 0, height = 0;
        renderer->GetSize(width, height);
        if (width <= 0 || height <= 0) {
            return;
        }
        cubismRenderer->SetRenderTargetSize(static_cast<Csm::csmUint32>(width),
                                            static_cast<Csm::csmUint32>(height));

        cubismRenderer->StartFrame(context);

        // 必须每帧通过 SetMvpMatrix 提交 MVP：渲染器内部的矩阵默认为单位阵，
        // 而模型顶点位于画布像素坐标系，不提交会因超出裁剪空间而不可见。
        Csm::CubismModelMatrix* const modelMatrix = currentModel_->GetModelMatrix();
        if (modelMatrix) {
            Csm::CubismMatrix44 projection;
            if (width < height) {
                // 竖长窗口：以模型宽度适配，横向按窗口比例压缩
                projection.Scale(1.0f, static_cast<float>(width) / static_cast<float>(height));
            } else {
                // 横长窗口：以模型高度适配，纵向按窗口比例压缩
                projection.Scale(static_cast<float>(height) / static_cast<float>(width), 1.0f);
            }
            projection.MultiplyByMatrix(modelMatrix);
            cubismRenderer->SetMvpMatrix(&projection);
        }

        cubismRenderer->DrawModel();
        cubismRenderer->EndFrame();
    }

    bool Live2DManager::HitTest(int x, int y) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!currentModel_) {
            return false;
        }

        // 将窗口坐标转换为模型坐标
        float modelX = static_cast<float>(x);
        float modelY = static_cast<float>(y);

        // 获取模型位置和缩放
        float posX, posY;
        currentModel_->GetPosition(posX, posY);
        float scale = currentModel_->GetScale();

        // 考虑窗口的物理坐标转换（此处简化）
        // 实际需要根据窗口大小和模型尺寸进行计算
        // 这里调用模型的HitTest方法
        return currentModel_->HitTest(modelX, modelY);
    }

    void Live2DManager::SetModelPosition(float x, float y) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetPosition(x, y);
            // 保存位置到配置
            ConfigManager::Instance().SetDouble("model.position_x", x);
            ConfigManager::Instance().SetDouble("model.position_y", y);
        }
    }

    void Live2DManager::GetModelPosition(float& x, float& y) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->GetPosition(x, y);
        }
        else {
            x = 0.0f;
            y = 0.0f;
        }
    }

    void Live2DManager::SetModelScale(float scale) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            scale = std::clamp(scale, 0.5f, 2.0f);
            currentModel_->SetScale(scale);
            ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, scale);
        }
    }

    float Live2DManager::GetModelScale() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            return currentModel_->GetScale();
        }
        return 1.0f;
    }

    void Live2DManager::SetModelOpacity(float opacity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            opacity = std::clamp(opacity, 0.2f, 1.0f);
            currentModel_->SetOpacity(opacity);
            ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, opacity);
        }
    }

    float Live2DManager::GetModelOpacity() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            return currentModel_->GetOpacity();
        }
        return 1.0f;
    }

    bool Live2DManager::PlayExpression(const std::string& expressionName) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!currentModel_) {
            LOG_WARN("Live2DManager", "No current model to play expression.");
            return false;
        }

        return currentModel_->PlayExpression(expressionName);
    }

    bool Live2DManager::PlayMotion(const std::string& motionName, bool loop) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!currentModel_) {
            LOG_WARN("Live2DManager", "No current model to play motion.");
            return false;
        }

        return currentModel_->PlayMotion(motionName, loop);
    }

    void Live2DManager::StopAllMotions() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->StopAllMotions();
        }
    }

    void Live2DManager::SetAutoBlink(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetAutoBlink(enabled);
        }
    }

    void Live2DManager::SetBreathEffect(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetBreathEffect(enabled);
        }
    }

    void Live2DManager::SetLipSync(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetLipSync(enabled);
        }
    }

    void Live2DManager::SetMouthOpen(float value) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetMouthOpen(value);
        }
    }
    void Live2DManager::SaveModelState() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!currentModel_) {
            return;
        }

        float x, y;
        currentModel_->GetPosition(x, y);
        float scale = currentModel_->GetScale();
        float opacity = currentModel_->GetOpacity();

        ConfigManager::Instance().SetDouble("model.position_x", x);
        ConfigManager::Instance().SetDouble("model.position_y", y);
        ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, scale);
        ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, opacity);
        ConfigManager::Instance().SetString(ConfigManager::KEY_MODEL_DEFAULT_ID, currentModelId_);

        LOG_DEBUG("Live2DManager", "Model state saved.");
    }

    void Live2DManager::RestoreModelState() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        std::string modelId = ConfigManager::Instance().GetString(ConfigManager::KEY_MODEL_DEFAULT_ID, "Haru");
        float x = ConfigManager::Instance().GetDouble("model.position_x", 0.0);
        float y = ConfigManager::Instance().GetDouble("model.position_y", 0.0);
        float scale = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, 1.0);
        float opacity = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, 1.0);

        // 这里**不加载**：首次加载要编译 Live2D 着色器（本机实测二十多秒），
        // 放在 Initialize 里会让中枢窗口迟迟出不来。参数的恢复照旧，
        // 真正的加载交给 AppCore 在中枢起来之后于渲染线程上执行。
        if (!IsModelLoaded(modelId)) {
            LOG_INFO("Live2DManager", "Model load deferred to post-startup: " + modelId);
        }

        if (currentModel_) {
            currentModel_->SetPosition(x, y);
            currentModel_->SetScale(scale);
            currentModel_->SetOpacity(opacity);
            LOG_DEBUG("Live2DManager", "Model state restored.");
        }
    }

    void Live2DManager::RequestModelSwitch(const std::string& modelId) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pendingSwitchId_ = modelId;
        LOG_INFO("Live2DManager", "Model switch queued for render thread: " + modelId);
    }

    std::string Live2DManager::TakePendingSwitch() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::string id;
        id.swap(pendingSwitchId_);
        return id;
    }

    std::string Live2DManager::GetCurrentModelId() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return currentModelId_;
    }

    std::vector<Live2DManager::ModelCapability> Live2DManager::ListModelCapabilities() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        std::vector<ModelCapability> result;
        const std::filesystem::path root = Utils::ResolveAppRelative(L"models");
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) {
            LOG_WARN("Live2DManager", "Model directory not found while listing capabilities.");
            return result;
        }

        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec || !entry.is_directory()) {
                continue;
            }
            const std::wstring dirName = entry.path().filename().wstring();
            const std::filesystem::path modelJson = entry.path() / (dirName + L".model3.json");
            if (!std::filesystem::exists(modelJson, ec)) {
                continue;   // 不是模型目录
            }

            ModelCapability caps;
            caps.id = Utils::PathToUtf8(dirName);
            caps.loaded = IsModelLoaded(caps.id);
            caps.current = (caps.id == currentModelId_);

            // 读 model3.json：动作组与表情清单
            try {
                std::ifstream in(modelJson);
                nlohmann::json doc;
                in >> doc;
                const nlohmann::json refs = doc.value("FileReferences", nlohmann::json::object());
                if (refs.contains("Motions") && refs["Motions"].is_object()) {
                    for (auto it = refs["Motions"].begin(); it != refs["Motions"].end(); ++it) {
                        caps.motions.push_back(it.key());
                    }
                }
                if (refs.contains("Expressions") && refs["Expressions"].is_array()) {
                    for (const auto& e : refs["Expressions"]) {
                        if (e.contains("Name")) {
                            caps.expressions.push_back(e["Name"].get<std::string>());
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_WARN("Live2DManager", std::string("Failed to parse model3.json: ") + e.what());
            }

            // 读 cdi3.json：参数 id 清单（有些模型没有这个文件）
            try {
                const std::filesystem::path cdi = entry.path() / (dirName + L".cdi3.json");
                if (std::filesystem::exists(cdi, ec)) {
                    std::ifstream in(cdi);
                    nlohmann::json doc;
                    in >> doc;
                    if (doc.contains("Parameters") && doc["Parameters"].is_array()) {
                        for (const auto& p : doc["Parameters"]) {
                            if (p.contains("Id")) {
                                caps.parameters.push_back(p["Id"].get<std::string>());
                                caps.parameterNames.push_back(
                                    p.contains("Name") ? p["Name"].get<std::string>() : std::string());
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_WARN("Live2DManager", std::string("Failed to parse cdi3.json: ") + e.what());
            }

            result.push_back(std::move(caps));
        }

        std::sort(result.begin(), result.end(),
            [](const ModelCapability& a, const ModelCapability& b) { return a.id < b.id; });
        return result;
    }

    bool Live2DManager::SwitchModelById(const std::string& modelId) {
        if (modelId.empty()) {
            return false;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (!IsModelLoaded(modelId)) {
                const std::filesystem::path root = Utils::ResolveAppRelative(L"models");
                const std::wstring wide(modelId.begin(), modelId.end());
                const std::filesystem::path modelJson = root / wide / (wide + L".model3.json");
                std::error_code ec;
                if (!std::filesystem::exists(modelJson, ec)) {
                    LOG_WARN("Live2DManager", "SwitchModelById: model not found: " + modelId);
                    return false;
                }
                // 加载会自动把新模型切成当前模型
                if (!LoadModel(modelId, modelJson.wstring())) {
                    return false;
                }
            }
        }

        if (!SwitchModel(modelId)) {
            return false;
        }

        // 只留当前模型：换模型不该把内存越吃越多（wuwuwu 这种大贴图尤其明显）
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            std::vector<std::string> stale;
            for (const auto& pair : models_) {
                if (pair.first != modelId) {
                    stale.push_back(pair.first);
                }
            }
            for (const auto& id : stale) {
                UnloadModel(id);
            }
            if (!stale.empty()) {
                LOG_INFO("Live2DManager", "Unloaded " + std::to_string(stale.size()) +
                    " inactive model(s) after switching.");
            }
        }
        return true;
    }

    void Live2DManager::ScanModelDirectory() {
        // 扫描模型目录，自动加载所有模型
        if (!std::filesystem::exists(modelDirectory_)) {
            LOG_WARN("Live2DManager", "Model directory not found: " + Utils::PathToUtf8(modelDirectory_));
            return;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(modelDirectory_)) {
                if (entry.is_directory()) {
                    std::string modelName = Utils::PathToUtf8(entry.path().filename());
                    std::wstring modelPath = entry.path() / (entry.path().filename().wstring() + L".model3.json");
                    if (std::filesystem::exists(modelPath)) {
                        LOG_INFO("Live2DManager", "Found model: " + modelName + " at " + Utils::PathToUtf8(modelPath));
                        // 只加载配置里选定的那个模型。以前是把目录下所有模型都载进来，
                        // 于是 Haru 与 wuwuwu 同时常驻 —— 后者是 8192x8192 贴图，
                        // 光这一项就白吃两百多 MB 内存/显存，老设备上很致命。
                        // 这里**不再加载**：首次编译 Live2D 着色器要十几秒，
                        // 放在 Initialize 里做等于让用户对着加载画面干等。
                        // 真正的加载由 AppCore 在中枢窗口起来之后、于渲染线程上执行
                        // （见 AppCore::pendingModelLoad_ 与 LoadDefaultModel）。
                        const std::string wanted = ConfigManager::Instance().GetString(
                            ConfigManager::KEY_MODEL_DEFAULT_ID, "");
                        if (wanted.empty() || modelName == wanted) {
                            LOG_INFO("Live2DManager", "Model deferred (will load after startup): " + modelName);
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("Live2DManager", "Failed to scan model directory: " + std::string(e.what()));
        }
    }

    void Live2DManager::ReleaseModelResources(const std::string& modelId) {
        auto it = models_.find(modelId);
        if (it != models_.end()) {
            it->second->Release();
        }
    }

    bool Live2DManager::LoadModelResources(const std::string& modelId, const std::wstring& modelPath) {
        // 实际加载在Model::Load中完成
        return true;
    }

} // namespace UnnamedBot