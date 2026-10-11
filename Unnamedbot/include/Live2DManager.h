#pragma once

#include "../include/Model.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace UnnamedBot {

    class Renderer;

    // Live2D模型管理器 - 管理所有模型实例
    class Live2DManager {
    public:
        static Live2DManager& Instance();

        // 初始化Live2D框架（必须在加载模型前调用，需要传入已初始化的 D3D11 渲染器）
        bool Initialize(Renderer* renderer);

        // 销毁Live2D框架
        void Shutdown();

        // 加载模型
        bool LoadModel(const std::string& modelId, const std::wstring& modelPath);

        // 卸载模型
        void UnloadModel(const std::string& modelId);

        // 切换当前模型
        bool SwitchModel(const std::string& modelId);

        // 获取当前模型
        std::shared_ptr<Model> GetCurrentModel() const { return currentModel_; }

        // 获取所有已加载模型ID列表
        std::vector<std::string> GetLoadedModelIds() const;

        // ---- 让 AI 自主驱动自己：任意导入模型的能力清单与切换 ----
        // 能力不写死在代码里，而是逐个读模型自带的 model3.json / cdi3.json，
        // 所以用户往 models\ 里丢任何模型，AI 都能立刻知道它有哪些动作、表情、参数。
        struct ModelCapability {
            std::string id;
            std::vector<std::string> motions;      // 动作组名（model3.json 的 FileReferences.Motions 键）
            std::vector<std::string> expressions;  // 表情名
            std::vector<std::string> parameters;   // 参数 id（模型带 cdi3.json 时才有）
            std::vector<std::string> parameterNames;  // 与 parameters 一一对应的显示名（便于按语义挑）
            bool loaded = false;
            bool current = false;
        };
        std::vector<ModelCapability> ListModelCapabilities();

        // 按 id 切换：没加载过就按扫描到的路径加载（运行中新放进来的模型也能直接用）
        bool SwitchModelById(const std::string& modelId);

        // 当前模型 id（没有模型时为空串）
        std::string GetCurrentModelId() const;

        // 检查模型是否已加载
        bool IsModelLoaded(const std::string& modelId) const;

        // 更新当前模型（每帧调用）
        void Update(float deltaTime);

        // 渲染当前模型
        void Render(Renderer* renderer);

        // 碰撞检测（像素级）
        bool HitTest(int x, int y) const;

        // 设置模型位置
        void SetModelPosition(float x, float y);

        // 获取模型位置
        void GetModelPosition(float& x, float& y) const;

        // 设置模型缩放
        void SetModelScale(float scale);

        // 获取模型缩放
        float GetModelScale() const;

        // 设置模型透明度
        void SetModelOpacity(float opacity);

        // 获取模型透明度
        float GetModelOpacity() const;

        // 播放表情
        bool PlayExpression(const std::string& expressionName);

        // 播放动作
        bool PlayMotion(const std::string& motionName, bool loop = false);

        // 停止所有动作
        void StopAllMotions();

        // 设置自动眨眼
        void SetAutoBlink(bool enabled);

        // 设置呼吸效果
        void SetBreathEffect(bool enabled);

        // 设置唇形同步
        void SetLipSync(bool enabled);

        // 保存当前模型状态
        void SaveModelState();

        // 恢复模型状态
        void RestoreModelState();

    private:
        Live2DManager() = default;
        ~Live2DManager() = default;
        Live2DManager(const Live2DManager&) = delete;
        Live2DManager& operator=(const Live2DManager&) = delete;

        // 扫描模型目录
        void ScanModelDirectory();

        // 加载模型资源
        bool LoadModelResources(const std::string& modelId, const std::wstring& modelPath);

        // 释放模型资源
        void ReleaseModelResources(const std::string& modelId);

        // 模型存储
        std::unordered_map<std::string, std::shared_ptr<Model>> models_;
        std::shared_ptr<Model> currentModel_ = nullptr;
        std::string currentModelId_;

        // 渲染设备（Initialize 时从 Renderer 取得），加载模型贴图时需要
        ID3D11Device* device_ = nullptr;

        // Live2D框架是否已初始化
        bool initialized_ = false;

        // 模型目录
        std::wstring modelDirectory_ = L"./models";

        // 互斥锁
        // 说明：本类的公开方法之间会互相调用（例如 Initialize 会调用
        // RestoreModelState/ScanModelDirectory，后者又调用 LoadModel，
        // 而 LoadModel 内部还会调用 IsModelLoaded），因此必须使用可重入锁，
        // 否则同一线程会重复加锁导致启动时死锁。
        mutable std::recursive_mutex mutex_;
    };

} // namespace UnnamedBot