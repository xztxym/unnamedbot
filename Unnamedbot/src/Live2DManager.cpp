#include "../include/Live2DManager.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Renderer.h"
#include <CubismFramework.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace UnnamedBot {

    Live2DManager& Live2DManager::Instance() {
        static Live2DManager instance;
        return instance;
    }

    bool Live2DManager::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("Live2DManager", "Live2D framework already initialized.");
            return true;
        }

        // 初始化Live2D框架
        Csm::CubismFramework::Initialize();

        // 设置日志回调
        Csm::CubismFramework::SetLogFunction([](Csm::csmLogLevel level, const Csm::csmChar* message) {
            std::string msg = message ? message : "";
            switch (level) {
            case Csm::csmLogLevel_Verbose:
                LOG_DEBUG("Live2D", msg);
                break;
            case Csm::csmLogLevel_Info:
                LOG_INFO("Live2D", msg);
                break;
            case Csm::csmLogLevel_Warning:
                LOG_WARN("Live2D", msg);
                break;
            case Csm::csmLogLevel_Error:
                LOG_ERROR("Live2D", msg);
                break;
            }
            });

        initialized_ = true;
        LOG_INFO("Live2DManager", "Live2D framework initialized.");

        // 扫描模型目录
        ScanModelDirectory();

        // 恢复模型状态
        RestoreModelState();

        return true;
    }

    void Live2DManager::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("Live2DManager", "Live2D framework not initialized.");
            return false;
        }

        if (IsModelLoaded(modelId)) {
            LOG_WARN("Live2DManager", "Model already loaded: " + modelId);
            return true;
        }

        LOG_INFO("Live2DManager", "Loading model: " + modelId + " from " + std::filesystem::path(modelPath).u8string());

        // 创建模型实例
        auto model = std::make_shared<Model>();
        if (!model->Load(modelPath)) {
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
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("Live2DManager", "Live2D framework not initialized.");
            return false;
        }

        auto it = models_.find(modelId);
        if (it == models_.end()) {
            LOG_ERROR("Live2DManager", "Model not found: " + modelId);
            return false;
        }

        // 释放当前模型资源
        if (currentModel_) {
            ReleaseModelResources(currentModelId_);
            currentModel_ = nullptr;
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
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (const auto& pair : models_) {
            ids.push_back(pair.first);
        }
        return ids;
    }

    bool Live2DManager::IsModelLoaded(const std::string& modelId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return models_.find(modelId) != models_.end();
    }

    void Live2DManager::Update(float deltaTime) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->Update(deltaTime);
        }
    }

    void Live2DManager::Render(Renderer* renderer) {
        std::lock_guard<std::mutex> lock(mutex_);

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
        if (renderTarget) {
            // 使用Live2D渲染器绘制
            cubismRenderer->DrawModel();
        }
    }

    bool Live2DManager::HitTest(int x, int y) const {
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetPosition(x, y);
            // 保存位置到配置
            ConfigManager::Instance().SetDouble("model.position_x", x);
            ConfigManager::Instance().SetDouble("model.position_y", y);
        }
    }

    void Live2DManager::GetModelPosition(float& x, float& y) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->GetPosition(x, y);
        }
        else {
            x = 0.0f;
            y = 0.0f;
        }
    }

    void Live2DManager::SetModelScale(float scale) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            scale = std::clamp(scale, 0.5f, 2.0f);
            currentModel_->SetScale(scale);
            ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, scale);
        }
    }

    float Live2DManager::GetModelScale() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            return currentModel_->GetScale();
        }
        return 1.0f;
    }

    void Live2DManager::SetModelOpacity(float opacity) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            opacity = std::clamp(opacity, 0.2f, 1.0f);
            currentModel_->SetOpacity(opacity);
            ConfigManager::Instance().SetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, opacity);
        }
    }

    float Live2DManager::GetModelOpacity() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            return currentModel_->GetOpacity();
        }
        return 1.0f;
    }

    bool Live2DManager::PlayExpression(const std::string& expressionName) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!currentModel_) {
            LOG_WARN("Live2DManager", "No current model to play expression.");
            return false;
        }

        return currentModel_->PlayExpression(expressionName);
    }

    bool Live2DManager::PlayMotion(const std::string& motionName, bool loop) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!currentModel_) {
            LOG_WARN("Live2DManager", "No current model to play motion.");
            return false;
        }

        return currentModel_->PlayMotion(motionName, loop);
    }

    void Live2DManager::StopAllMotions() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->StopAllMotions();
        }
    }

    void Live2DManager::SetAutoBlink(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetAutoBlink(enabled);
        }
    }

    void Live2DManager::SetBreathEffect(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetBreathEffect(enabled);
        }
    }

    void Live2DManager::SetLipSync(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentModel_) {
            currentModel_->SetLipSync(enabled);
        }
    }

    void Live2DManager::SaveModelState() {
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

        std::string modelId = ConfigManager::Instance().GetString(ConfigManager::KEY_MODEL_DEFAULT_ID, "Haru");
        float x = ConfigManager::Instance().GetDouble("model.position_x", 0.0);
        float y = ConfigManager::Instance().GetDouble("model.position_y", 0.0);
        float scale = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_SCALE, 1.0);
        float opacity = ConfigManager::Instance().GetDouble(ConfigManager::KEY_MODEL_DEFAULT_OPACITY, 1.0);

        // 如果模型未加载，尝试加载
        if (!IsModelLoaded(modelId)) {
            // 扫描模型目录查找该模型
            std::wstring modelPath = modelDirectory_ + L"/" + std::wstring(modelId.begin(), modelId.end()) + L"/" +
                std::wstring(modelId.begin(), modelId.end()) + L".model3.json";
            if (std::filesystem::exists(modelPath)) {
                LoadModel(modelId, modelPath);
            }
            else {
                // 尝试加载默认模型（如果存在）
                LOG_WARN("Live2DManager", "Model not found: " + modelId + ", trying to load Haru.");
                std::wstring haruPath = modelDirectory_ + L"/Haru/Haru.model3.json";
                if (std::filesystem::exists(haruPath)) {
                    LoadModel("Haru", haruPath);
                }
                else {
                    LOG_ERROR("Live2DManager", "No default model found.");
                    return;
                }
            }
        }

        if (currentModel_) {
            currentModel_->SetPosition(x, y);
            currentModel_->SetScale(scale);
            currentModel_->SetOpacity(opacity);
            LOG_DEBUG("Live2DManager", "Model state restored.");
        }
    }

    void Live2DManager::ScanModelDirectory() {
        // 扫描模型目录，自动加载所有模型
        if (!std::filesystem::exists(modelDirectory_)) {
            LOG_WARN("Live2DManager", "Model directory not found: " + std::filesystem::path(modelDirectory_).u8string());
            return;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(modelDirectory_)) {
                if (entry.is_directory()) {
                    std::string modelName = entry.path().filename().u8string();
                    std::wstring modelPath = entry.path() / (entry.path().filename().wstring() + L".model3.json");
                    if (std::filesystem::exists(modelPath)) {
                        LOG_INFO("Live2DManager", "Found model: " + modelName + " at " + std::filesystem::path(modelPath).u8string());
                        // 加载模型（但不自动切换）
                        LoadModel(modelName, modelPath);
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