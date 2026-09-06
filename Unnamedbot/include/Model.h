#pragma once

#include <CubismFramework.hpp>
#include <Model/CubismUserModel.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <string>
#include <memory>

namespace UnnamedBot {

    // Live2D模型封装
    class Model {
    public:
        Model();
        ~Model();

        // 加载模型
        bool Load(const std::wstring& modelPath);

        // 释放模型
        void Release();

        // 更新模型
        void Update(float deltaTime);

        // 获取渲染器
        Csm::Rendering::CubismRenderer_D3D11* GetRenderer() const;

        // 碰撞检测
        bool HitTest(float x, float y) const;

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

        // 设置位置
        void SetPosition(float x, float y);

        // 获取位置
        void GetPosition(float& x, float& y) const;

        // 设置缩放
        void SetScale(float scale);

        // 获取缩放
        float GetScale() const;

        // 设置透明度
        void SetOpacity(float opacity);

        // 获取透明度
        float GetOpacity() const;

        // 设置参数值
        void SetParameterValue(const std::string& parameterId, float value);

        // 获取参数值
        float GetParameterValue(const std::string& parameterId) const;

        // 设置部件不透明度
        void SetPartOpacity(const std::string& partId, float opacity);

        // 获取部件不透明度
        float GetPartOpacity(const std::string& partId) const;

        // 重置参数（清除所有动态参数）
        void ResetParameters();

        // 保存模型状态
        void SaveState();

        // 恢复模型状态
        void RestoreState();

        // 创建渲染器
        void CreateRenderer();

    private:
        // 加载模型数据
        bool LoadModelData(const std::wstring& modelPath);

        // 加载表情文件
        void LoadExpressions(const std::wstring& modelPath);

        // 加载动作文件
        void LoadMotions(const std::wstring& modelPath);

        // 设置初始参数
        void SetInitialParameters();

        // Live2D用户模型
        std::unique_ptr<Csm::CubismUserModel> userModel_;

        // 渲染器
        Csm::Rendering::CubismRenderer_D3D11* renderer_ = nullptr;

        // 模型状态
        float positionX_ = 0.0f;
        float positionY_ = 0.0f;
        float scale_ = 1.0f;
        float opacity_ = 1.0f;

        // 功能开关
        bool autoBlink_ = true;
        bool breathEffect_ = true;
        bool lipSync_ = false;

        // 表情列表
        std::vector<std::string> expressionNames_;
        std::vector<std::string> motionNames_;

        // 是否已加载
        bool loaded_ = false;

        // 互斥锁
        mutable std::mutex mutex_;
    };

} // namespace UnnamedBot