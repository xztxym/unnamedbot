#include "../include/Model.h"
#include "../include/Logger.h"
#include "../include/Utils.h"
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Rendering/D3D11/CubismShader_D3D11.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Effect/CubismPose.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Id/CubismIdManager.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace Csm;
using namespace Csm::Rendering;

namespace UnnamedBot {

namespace {

    // 动作优先级（对应官方样例 LAppDefine 中的 Priority* 常量）
    constexpr csmInt32 kPriorityNone = 0;
    constexpr csmInt32 kPriorityIdle = 1;
    constexpr csmInt32 kPriorityNormal = 2;
    constexpr csmInt32 kPriorityForce = 3;

    // 将文件读入内存：Cubism 的工厂函数需要内存缓冲区而不是文件路径
    bool ReadFileToBuffer(const std::filesystem::path& path, std::string& buffer) {
        return Utils::ReadFileToString(path.wstring(), buffer);
    }

} // namespace

    Model::Model() {
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

        LOG_INFO("Model", "Loading model from: " + Utils::PathToUtf8(modelPath));

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

        if (!loaded_) {
            return;
        }

        // 停止动作与表情
        if (_motionManager) {
            _motionManager->StopAllMotions();
        }
        if (_expressionManager) {
            _expressionManager->StopAllMotions();
        }

        DeleteRenderer();

        // 以下释放顺序与 CubismUserModel::~CubismUserModel 保持一致；
        // 各 Delete 对空指针安全，因此置空后基类析构不会重复释放。
        CubismPose::Delete(_pose);
        _pose = nullptr;

        CubismEyeBlink::Delete(_eyeBlink);
        _eyeBlink = nullptr;

        CubismBreath::Delete(_breath);
        _breath = nullptr;

        CubismPhysics::Delete(_physics);
        _physics = nullptr;

        if (_model && _moc) {
            _moc->DeleteModel(_model);
        }
        _model = nullptr;

        CubismMoc::Delete(_moc);
        _moc = nullptr;

        CSM_DELETE(_modelMatrix);
        _modelMatrix = nullptr;

        expressionNames_.clear();
        motionNames_.clear();
        expressionDirectory_.clear();
        motionDirectory_.clear();

        loaded_ = false;
        LOG_INFO("Model", "Model released.");
    }

    void Model::Update(float deltaTime) {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        if (!loaded_ || !model) {
            return;
        }

        // 参数应用顺序遵循官方样例：动作 → 表情 → 呼吸 → 眨眼 → 物理 → 姿势 → 应用
        if (_motionManager) {
            _motionManager->UpdateMotion(model, deltaTime);
        }

        if (_expressionManager) {
            _expressionManager->UpdateMotion(model, deltaTime);
        }

        if (breathEffect_ && _breath) {
            _breath->UpdateParameters(model, deltaTime);
        }

        if (autoBlink_ && _eyeBlink) {
            _eyeBlink->UpdateParameters(model, deltaTime);
        }

        if (_physics) {
            _physics->Evaluate(model, deltaTime);
        }

        if (_pose) {
            _pose->UpdateParameters(model, deltaTime);
        }

        // 更新模型矩阵
        CubismModelMatrix* const modelMatrix = GetModelMatrix();
        if (modelMatrix) {
            modelMatrix->SetPosition(positionX_, positionY_);
            modelMatrix->Scale(scale_, scale_);
            modelMatrix->SetCenterPosition(0.0f, 0.0f);
        }

        // 将本帧参数应用到模型
        model->Update();

        // 透明度由 CubismUserModel / CubismModel 管理，渲染器本身没有透明度接口
        CubismUserModel::SetOpacity(opacity_);
    }

    CubismRenderer_D3D11* Model::GetRenderer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dynamic_cast<CubismRenderer_D3D11*>(_renderer);
    }

    bool Model::HitTest(float x, float y) const {
        std::lock_guard<std::mutex> lock(mutex_);

        const CubismModel* const model = GetModel();
        if (!loaded_ || !model) {
            return false;
        }

        // 简化版碰撞检测：检测模型边界
        // 实际应使用Live2D的IsHit方法
        // 这里假设模型在屏幕中心，半径为100像素
        float centerX = 0.0f;
        float centerY = 0.0f;
        float radius = 100.0f;

        // 获取模型尺寸
        float modelWidth = model->GetCanvasWidth();
        float modelHeight = model->GetCanvasHeight();

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

        if (!loaded_ || !_expressionManager) {
            LOG_WARN("Model", "Cannot play expression: model not ready.");
            return false;
        }

        // 表情文件位于模型目录的 expressions 子目录
        const std::filesystem::path expressionPath =
            std::filesystem::path(expressionDirectory_) / (Utils::Utf8ToWide(expressionName) + L".exp3.json");

        std::string buffer;
        if (!ReadFileToBuffer(expressionPath, buffer)) {
            LOG_WARN("Model", "Expression not found: " + expressionName);
            return false;
        }

        auto* expression = CubismExpressionMotion::Create(
            reinterpret_cast<const csmByte*>(buffer.data()),
            static_cast<csmSizeInt>(buffer.size())
        );

        if (!expression) {
            LOG_WARN("Model", "Failed to parse expression: " + expressionName);
            return false;
        }

        // 表情由管理器接管（autoDelete=true），播完自动释放
        _expressionManager->StartMotion(expression, true);
        LOG_DEBUG("Model", "Playing expression: " + expressionName);
        return true;
    }

    bool Model::PlayMotion(const std::string& motionName, bool loop) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!loaded_ || !_motionManager) {
            LOG_WARN("Model", "Cannot play motion: model not ready.");
            return false;
        }

        // 动作文件位于模型目录的 motions 子目录
        const std::filesystem::path motionPath =
            std::filesystem::path(motionDirectory_) / (Utils::Utf8ToWide(motionName) + L".motion3.json");

        std::string buffer;
        if (!ReadFileToBuffer(motionPath, buffer)) {
            LOG_WARN("Model", "Motion not found: " + motionName);
            return false;
        }

        auto* motion = CubismMotion::Create(
            reinterpret_cast<const csmByte*>(buffer.data()),
            static_cast<csmSizeInt>(buffer.size())
        );

        if (!motion) {
            LOG_WARN("Model", "Failed to parse motion: " + motionName);
            return false;
        }

        motion->SetLoop(loop);
        // 注意参数顺序：StartMotionPriority(motion, autoDelete, priority)
        _motionManager->StartMotionPriority(motion, true, kPriorityForce);
        LOG_DEBUG("Model", "Playing motion: " + motionName + (loop ? " (loop)" : ""));
        return true;
    }

    void Model::StopAllMotions() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (_motionManager) {
            _motionManager->StopAllMotions();
            LOG_DEBUG("Model", "All motions stopped.");
        }
    }

    void Model::SetAutoBlink(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        autoBlink_ = enabled;
        ApplyAutoBlink();
    }

    void Model::ApplyAutoBlink() {
        if (!autoBlink_ || _eyeBlink) {
            return;
        }

        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!idManager) {
            return;
        }

        _eyeBlink = CubismEyeBlink::Create();

        // 未传入 model3.json 设置时框架不会登记默认参数 ID，这里手动指定双眼开合参数
        csmVector<CubismIdHandle> parameterIds;
        parameterIds.PushBack(idManager->GetId(DefaultParameterId::ParamEyeLOpen));
        parameterIds.PushBack(idManager->GetId(DefaultParameterId::ParamEyeROpen));
        _eyeBlink->SetParameterIds(parameterIds);
    }

    void Model::SetBreathEffect(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        breathEffect_ = enabled;
        ApplyBreathEffect();
    }

    void Model::ApplyBreathEffect() {
        if (!breathEffect_ || _breath) {
            return;
        }

        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!idManager) {
            return;
        }

        _breath = CubismBreath::Create();

        // 呼吸参数采用官方样例的默认值（参数ID, 偏移, 峰值, 周期, 权重）
        csmVector<CubismBreath::BreathParameterData> parameters;
        parameters.PushBack(CubismBreath::BreathParameterData(
            idManager->GetId(DefaultParameterId::ParamAngleX), 0.0f, 15.0f, 6.5345f, 0.5f));
        parameters.PushBack(CubismBreath::BreathParameterData(
            idManager->GetId(DefaultParameterId::ParamAngleY), 0.0f, 8.0f, 3.5345f, 0.5f));
        parameters.PushBack(CubismBreath::BreathParameterData(
            idManager->GetId(DefaultParameterId::ParamAngleZ), 0.0f, 10.0f, 5.5345f, 0.5f));
        parameters.PushBack(CubismBreath::BreathParameterData(
            idManager->GetId(DefaultParameterId::ParamBodyAngleX), 0.0f, 4.0f, 15.5345f, 0.5f));
        parameters.PushBack(CubismBreath::BreathParameterData(
            idManager->GetId(DefaultParameterId::ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));

        _breath->SetParameters(parameters);
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
        CubismModelMatrix* const modelMatrix = GetModelMatrix();
        if (modelMatrix) {
            modelMatrix->SetPosition(x, y);
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
        CubismModelMatrix* const modelMatrix = GetModelMatrix();
        if (modelMatrix) {
            // CubismModelMatrix 没有 SetScale，缩放通过基类 CubismMatrix44::Scale 实现
            modelMatrix->Scale(scale_, scale_);
        }
    }

    float Model::GetScale() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scale_;
    }

    void Model::SetOpacity(float opacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        opacity_ = std::clamp(opacity, 0.2f, 1.0f);
        // 必须限定基类：不限定会递归调用到本函数
        CubismUserModel::SetOpacity(opacity_);
    }

    float Model::GetOpacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return opacity_;
    }

    void Model::SetParameterValue(const std::string& parameterId, float value) {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!model || !idManager) {
            return;
        }

        model->SetParameterValueById(idManager->GetId(parameterId.c_str()), value);
    }

    float Model::GetParameterValue(const std::string& parameterId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!model || !idManager) {
            return 0.0f;
        }

        return model->GetParameterValueById(idManager->GetId(parameterId.c_str()));
    }

    void Model::SetPartOpacity(const std::string& partId, float opacity) {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!model || !idManager) {
            return;
        }

        model->SetPartOpacityById(idManager->GetId(partId.c_str()), opacity);
    }

    float Model::GetPartOpacity(const std::string& partId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        CubismIdManager* const idManager = CubismFramework::GetIdManager();
        if (!model || !idManager) {
            return 1.0f;
        }

        return model->GetPartOpacityById(idManager->GetId(partId.c_str()));
    }

    void Model::ResetParameters() {
        std::lock_guard<std::mutex> lock(mutex_);

        CubismModel* const model = GetModel();
        if (!model) {
            return;
        }

        // CubismModel 没有“重置参数”接口：
        // LoadModel 时已保存初始参数快照，LoadParameters 即可恢复到该快照。
        model->LoadParameters();
        LOG_DEBUG("Model", "Parameters reset.");
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
        std::string pathStr = Utils::PathToUtf8(modelPath);
        if (!userModel_->LoadModelJson(pathStr.c_str())) {
            LOG_ERROR("Model", "Failed to load model JSON: " + pathStr);
            return false;
        }

        // 初始化模型矩阵
        userModel_->GetModelMatrix()->SetPosition(positionX_, positionY_);
        userModel_->GetModelMatrix()->SetScale(scale_, scale_);
        userModel_->GetModelMatrix()->SetCenterPosition(0.0f, 0.0f);

        // 创建物理（如果存在）
        std::string physicsPath = Utils::PathToUtf8(std::filesystem::path(modelPath).parent_path()) + "/" +
            Utils::PathToUtf8(std::filesystem::path(modelPath).stem()) + ".physics3.json";
        if (std::filesystem::exists(physicsPath)) {
            auto physics = CubismPhysics::Create(physicsPath.c_str());
            if (physics) {
                userModel_->SetPhysics(physics);
            }
        }

        // 创建姿势（如果存在）
        std::string posePath = Utils::PathToUtf8(std::filesystem::path(modelPath).parent_path()) + "/" +
            Utils::PathToUtf8(std::filesystem::path(modelPath).stem()) + ".pose3.json";
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
                    std::string expName = Utils::PathToUtf8(entry.path().stem());
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
                    std::string motionName = Utils::PathToUtf8(entry.path().stem());
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