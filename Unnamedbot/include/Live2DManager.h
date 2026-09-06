#pragma once

#include "../include/Model.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace UnnamedBot {

    // Live2D模型管理器 - 管理所有模型实例
    class Live2DManager {
    public:
        static Live2DManager& Instance();

        // 初始化Live2D框架
        bool Initialize();

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

        // Live2D框架是否已初始化
        bool initialized_ = false;

        // 模型目录
        std::wstring modelDirectory_ = L"./models";

        // 互斥锁
        mutable std::mutex mutex_;
    };

} // namespace UnnamedBot