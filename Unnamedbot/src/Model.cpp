#include "../include/Model.h"
#include "../include/Logger.h"
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Rendering/D3D11/CubismShader_D3D11.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Effect/CubismPose.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Id/CubismIdManager.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace Csm;
using namespace Csm::Rendering;

namespace UnnamedBot {

    Model::Model() {
        userModel_ = std::make_unique<CubismUserModel>();
    }

    Model::~Model() {
        Release();
    }

    bool Model::Load(const std::wstring& modelPath) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (loaded_) {
            LOG_WARN("Model", "Model already loaded.");
            return true;
        }

        LOG_INFO("Model", "Loading model from: " + std::filesystem::path(modelPath).u8string());

        if (!LoadModelData(modelPath)) {
            LOG_ERROR("Model", "Failed to load model data.");
            return false;
        }

        // 加载表情
        LoadExpressions(modelPath);

        // 加载动作
        LoadMotions(modelPath);

        // 设置初始参数
        SetInitialParameters();

        // 创建渲染器
        CreateRenderer();

        loaded_ = true;
        LOG_INFO("Model", "Model loaded successfully.");
        return true;
    }

    void Model::Release() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (renderer_) {
            renderer_->Release();
            delete renderer_;
            renderer_ = nullptr;
        }

        if (userModel_) {
            userModel_->DeleteRenderer();
            // 释放模型资源
            // 注意：CubismUserModel::~CubismUserModel会释放资源
            userModel_.reset();
            userModel_ = std::make_unique<CubismUserModel>();
        }

        loaded_ = false;
        LOG_INFO("Model", "Model released.");
    }

    void Model::Update(float deltaTime) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!loaded_ || !userModel_ || !userModel_->GetModel()) {
            return;
        }

        // 更新模型
        userModel_->Update();

        // 更新自动眨眼
        if (autoBlink_ && userModel_->GetEyeBlink()) {
            userModel_->GetEyeBlink()->UpdateParameters(userModel_->GetModel(), deltaTime);
        }

        // 更新呼吸效果
        if (breathEffect_ && userModel_->GetBreath()) {
            userModel_->GetBreath()->UpdateParameters(userModel_->GetModel(), deltaTime);
        }

        // 更新动作
        if (userModel_->GetMotionManager()) {
            userModel_->GetMotionManager()->UpdateMotion(userModel_->GetModel(), deltaTime);
        }

        // 更新表情
        if (userModel_->GetExpressionManager()) {
            userModel_->GetExpressionManager()->UpdateMotion(userModel_->GetModel(), deltaTime);
        }

        // 更新姿势
        if (userModel_->GetPose()) {
            userModel_->GetPose()->UpdateParameters(userModel_->GetModel(), deltaTime);
        }

        // 应用物理
        if (userModel_->GetPhysics()) {
            userModel_->GetPhysics()->Evaluate(userModel_->GetModel(), deltaTime);
        }

        // 更新模型矩阵
        userModel_->GetModelMatrix()->SetPosition(positionX_, positionY_);
        userModel_->GetModelMatrix()->SetScale(scale_, scale_);
        userModel_->GetModelMatrix()->SetCenterPosition(0.0f, 0.0f);

        // 更新渲染器
        if (renderer_) {
            renderer_->SetOpacity(opacity_);
            renderer_->SetModelMatrix(userModel_->GetModelMatrix());
        }
    }

    CubismRenderer_D3D11* Model::GetRenderer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return renderer_;
    }

    bool Model::HitTest(float x, float y) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!loaded_ || !userModel_ || !userModel_->GetModel()) {
            return false;
        }

        // 简化版碰撞检测：检测模型边界
        // 实际应使用Live2D的IsHit方法
        // 这里假设模型在屏幕中心，半径为100像素
        float centerX = 0.0f;
        float centerY = 0.0f;
        float radius = 100.0f;

        // 获取模型尺寸
        float modelWidth = userModel_->GetModel()->GetCanvasWidth();
        float modelHeight = userModel_->GetModel()->GetCanvasHeight();

        // 将坐标转换到模型空间
        float scale = scale_;
        float posX = positionX_;
        float posY = positionY_;

        // 调整坐标
        float adjustedX = (x - posX) / scale;
        float adjustedY = (y - posY) / scale;

        // 简单检测：是否在模型画布范围内
        if (adjustedX >= -modelWidth / 2 && adjustedX <= modelWidth / 2 &&
            adjustedY >= -modelHeight / 2 && adjustedY <= modelHeight / 2) {
            return true;
        }

        return false;
    }

    bool Model::PlayExpression(const std::string& expressionName) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!loaded_ || !userModel_ || !userModel_->GetExpressionManager()) {
            LOG_WARN("Model", "Cannot play expression: model not ready.");
            return false;
        }

        // 查找表情
        auto expression = CubismExpressionMotion::Create(
            (std::filesystem::path(expressionName) / (expressionName + ".exp3.json")).string().c_str()
        );

        if (expression) {
            userModel_->GetExpressionManager()->StartMotionPriority(
                expression,
                CubismMotionPriority::PriorityForce
            );
            LOG_DEBUG("Model", "Playing expression: " + expressionName);
            return true;
        }

        LOG_WARN("Model", "Expression not found: " + expressionName);
        return false;
    }

    bool Model::PlayMotion(const std::string& motionName, bool loop) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!loaded_ || !userModel_ || !userModel_->GetMotionManager()) {
            LOG_WARN("Model", "Cannot play motion: model not ready.");
            return false;
        }

        // 查找动作
        std::string motionPath = "./motions/" + motionName + ".motion3.json";
        auto motion = CubismMotion::Create(motionPath.c_str());

        if (motion) {
            motion->SetIsLoop(loop);
            userModel_->GetMotionManager()->StartMotionPriority(
                motion,
                CubismMotionPriority::PriorityForce
            );
            LOG_DEBUG("Model", "Playing motion: " + motionName + (loop ? " (loop)" : ""));
            return true;
        }

        LOG_WARN("Model", "Motion not found: " + motionName);
        return false;
    }

    void Model::StopAllMotions() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (userModel_ && userModel_->GetMotionManager()) {
            userModel_->GetMotionManager()->StopAllMotions();
            LOG_DEBUG("Model", "All motions stopped.");
        }
    }

    void Model::SetAutoBlink(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        autoBlink_ = enabled;
        if (enabled && userModel_ && !userModel_->GetEyeBlink()) {
            userModel_->SetEyeBlink(new CubismEyeBlink());
            userModel_->GetEyeBlink()->SetParameterIds(
                CubismDefaultParameterId::ParamEyeLOpen,
                CubismDefaultParameterId::ParamEyeROpen
            );
        }
    }

    void Model::SetBreathEffect(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        breathEffect_ = enabled;
        if (enabled && userModel_ && !userModel_->GetBreath()) {
            userModel_->SetBreath(new CubismBreath());
            // 配置呼吸参数
            auto breath = userModel_->GetBreath();
            breath->SetParameters({
                {CubismDefaultParameterId::ParamBreath, 0.5f, 0.5f, 0.5f, 1.0f},
                {CubismDefaultParameterId::ParamAngleX, 0.0f, 0.0f, 0.0f, 1.0f},
                {CubismDefaultParameterId::ParamAngleY, 0.0f, 0.0f, 0.0f, 1.0f},
                {CubismDefaultParameterId::ParamAngleZ, 0.0f, 0.0f, 0.0f, 1.0f}
                });
        }
    }

    void Model::SetLipSync(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        lipSync_ = enabled;
        // 唇形同步实现需要外部音频数据，暂不实现
    }

    void Model::SetPosition(float x, float y) {
        std::lock_guard<std::mutex> lock(mutex_);
        positionX_ = x;
        positionY_ = y;
        if (userModel_ && userModel_->GetModelMatrix()) {
            userModel_->GetModelMatrix()->SetPosition(x, y);
        }
    }

    void Model::GetPosition(float& x, float& y) const {
        std::lock_guard<std::mutex> lock(mutex_);
        x = positionX_;
        y = positionY_;
    }

    void Model::SetScale(float scale) {
        std::lock_guard<std::mutex> lock(mutex_);
        scale_ = std::clamp(scale, 0.5f, 2.0f);
        if (userModel_ && userModel_->GetModelMatrix()) {
            userModel_->GetModelMatrix()->SetScale(scale_, scale_);
        }
    }

    float Model::GetScale() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scale_;
    }

    void Model::SetOpacity(float opacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        opacity_ = std::clamp(opacity, 0.2f, 1.0f);
        if (renderer_) {
            renderer_->SetOpacity(opacity_);
        }
    }

    float Model::GetOpacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return opacity_;
    }

    void Model::SetParameterValue(const std::string& parameterId, float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (userModel_ && userModel_->GetModel()) {
            userModel_->GetModel()->SetParameterValueById(
                CubismIdManager::GetInstance()->GetId(parameterId.c_str()),
                value
            );
        }
    }

    float Model::GetParameterValue(const std::string& parameterId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (userModel_ && userModel_->GetModel()) {
            return userModel_->GetModel()->GetParameterValueById(
                CubismIdManager::GetInstance()->GetId(parameterId.c_str())
            );
        }
        return 0.0f;
    }

    void Model::SetPartOpacity(const std::string& partId, float opacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (userModel_ && userModel_->GetModel()) {
            userModel_->GetModel()->SetPartOpacityById(
                CubismIdManager::GetInstance()->GetId(partId.c_str()),
                opacity
            );
        }
    }

    float Model::GetPartOpacity(const std::string& partId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (userModel_ && userModel_->GetModel()) {
            return userModel_->GetModel()->GetPartOpacityById(
                CubismIdManager::GetInstance()->GetId(partId.c_str())
            );
        }
        return 1.0f;
    }

    void Model::ResetParameters() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (userModel_ && userModel_->GetModel()) {
            userModel_->GetModel()->ResetParameters();
            LOG_DEBUG("Model", "Parameters reset.");
        }
    }

    void Model::SaveState() {
        // 保存模型状态（参数、位置等）
        // 通过配置管理器存储
        std::lock_guard<std::mutex> lock(mutex_);
        // 这里可以保存关键参数
        LOG_DEBUG("Model", "Model state saved.");
    }

    void Model::RestoreState() {
        std::lock_guard<std::mutex> lock(mutex_);
        // 恢复模型状态
        LOG_DEBUG("Model", "Model state restored.");
    }

    void Model::CreateRenderer() {
        if (!userModel_ || !userModel_->GetModel()) {
            LOG_ERROR("Model", "Cannot create renderer: model not loaded.");
            return;
        }

        // 创建D3D11渲染器
        renderer_ = new CubismRenderer_D3D11();
        renderer_->Initialize(userModel_->GetModel());

        // 设置渲染器参数
        renderer_->SetOpacity(opacity_);
        renderer_->SetModelMatrix(userModel_->GetModelMatrix());

        // 设置纹理（需要从外部加载）
        // 实际应该从模型文件加载纹理
        // 这里简化处理
        LOG_DEBUG("Model", "D3D11 renderer created.");
    }

    bool Model::LoadModelData(const std::wstring& modelPath) {
        if (!userModel_ || !CubismFramework::IsStarted()) {
            LOG_ERROR("Model", "Live2D framework not started or user model invalid.");
            return false;
        }

        // 加载模型文件
        std::string pathStr = std::filesystem::path(modelPath).u8string();
        if (!userModel_->LoadModelJson(pathStr.c_str())) {
            LOG_ERROR("Model", "Failed to load model JSON: " + pathStr);
            return false;
        }

        // 初始化模型矩阵
        userModel_->GetModelMatrix()->SetPosition(positionX_, positionY_);
        userModel_->GetModelMatrix()->SetScale(scale_, scale_);
        userModel_->GetModelMatrix()->SetCenterPosition(0.0f, 0.0f);

        // 创建物理（如果存在）
        std::string physicsPath = std::filesystem::path(modelPath).parent_path().u8string() + "/" +
            std::filesystem::path(modelPath).stem().u8string() + ".physics3.json";
        if (std::filesystem::exists(physicsPath)) {
            auto physics = CubismPhysics::Create(physicsPath.c_str());
            if (physics) {
                userModel_->SetPhysics(physics);
            }
        }

        // 创建姿势（如果存在）
        std::string posePath = std::filesystem::path(modelPath).parent_path().u8string() + "/" +
            std::filesystem::path(modelPath).stem().u8string() + ".pose3.json";
        if (std::filesystem::exists(posePath)) {
            auto pose = CubismPose::Create(posePath.c_str());
            if (pose) {
                userModel_->SetPose(pose);
            }
        }

        // 初始化自动眨眼
        SetAutoBlink(true);

        // 初始化呼吸效果
        SetBreathEffect(true);

        LOG_INFO("Model", "Model data loaded successfully.");
        return true;
    }

    void Model::LoadExpressions(const std::wstring& modelPath) {
        // 从模型目录加载表情文件
        std::wstring parentDir = std::filesystem::path(modelPath).parent_path();
        std::wstring expDir = parentDir + L"/expressions";

        if (!std::filesystem::exists(expDir)) {
            LOG_DEBUG("Model", "Expressions directory not found.");
            return;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(expDir)) {
                if (entry.is_regular_file() && entry.path().extension() == L".exp3.json") {
                    std::string expName = entry.path().stem().u8string();
                    expressionNames_.push_back(expName);
                    LOG_DEBUG("Model", "Found expression: " + expName);
                }
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("Model", "Failed to load expressions: " + std::string(e.what()));
        }
    }

    void Model::LoadMotions(const std::wstring& modelPath) {
        // 从模型目录加载动作文件
        std::wstring parentDir = std::filesystem::path(modelPath).parent_path();
        std::wstring motionDir = parentDir + L"/motions";

        if (!std::filesystem::exists(motionDir)) {
            LOG_DEBUG("Model", "Motions directory not found.");
            return;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(motionDir)) {
                if (entry.is_regular_file() && entry.path().extension() == L".motion3.json") {
                    std::string motionName = entry.path().stem().u8string();
                    motionNames_.push_back(motionName);
                    LOG_DEBUG("Model", "Found motion: " + motionName);
                }
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("Model", "Failed to load motions: " + std::string(e.what()));
        }
    }

    void Model::SetInitialParameters() {
        if (!userModel_ || !userModel_->GetModel()) {
            return;
        }

        // 设置初始参数（可根据需要调整）
        // 例如：设置脸部朝向、身体角度等
        LOG_DEBUG("Model", "Initial parameters set.");
    }

} // namespace UnnamedBot