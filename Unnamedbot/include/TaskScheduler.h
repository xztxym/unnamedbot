#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>

namespace UnnamedBot {

    // 定时任务
    struct ScheduledTask {
        int64_t id = 0;
        std::string name;
        std::string cronExpression;
        std::string actionType;    // "reminder", "custom"
        std::string actionData;    // JSON参数
        bool enabled = true;
        int64_t lastRun = 0;
        int64_t nextRun = 0;
        int64_t createdAt = 0;
        int64_t updatedAt = 0;
    };

    // 任务执行回调
    using TaskCallback = std::function<void(const ScheduledTask& task)>;

    // 任务调度器
    class TaskScheduler {
    public:
        static TaskScheduler& Instance();

        // 初始化
        void Initialize();

        // 关闭
        void Shutdown();

        // 添加任务
        int64_t AddTask(
            const std::string& name,
            const std::string& cronExpression,
            const std::string& actionType,
            const std::string& actionData = "{}"
        );

        // 删除任务
        bool DeleteTask(int64_t taskId);

        // 启用/禁用任务
        bool EnableTask(int64_t taskId, bool enabled);

        // 更新任务
        bool UpdateTask(int64_t taskId, const ScheduledTask& task);

        // 获取任务
        std::shared_ptr<ScheduledTask> GetTask(int64_t taskId) const;

        // 获取所有任务
        std::vector<std::shared_ptr<ScheduledTask>> GetAllTasks() const;

        // 获取启用的任务
        std::vector<std::shared_ptr<ScheduledTask>> GetEnabledTasks() const;

        // 立即执行任务
        bool ExecuteTask(int64_t taskId);

        // 更新任务调度
        void Update();

        // 注册任务回调
        void OnTaskExecute(TaskCallback callback);

        // 计算下次执行时间
        int64_t CalculateNextRun(const std::string& cronExpression, int64_t fromTime = 0) const;

    private:
        TaskScheduler() = default;
        ~TaskScheduler() = default;
        TaskScheduler(const TaskScheduler&) = delete;
        TaskScheduler& operator=(const TaskScheduler&) = delete;

        // 加载任务
        void LoadTasks();

        // 保存任务
        bool SaveTask(const ScheduledTask& task);

        // 检查并执行任务
        void CheckAndExecuteTasks();

        // 解析Cron表达式
        bool ParseCron(const std::string& cron, int& minute, int& hour, int& day, int& month, int& dayOfWeek);

        // 计算下一个匹配的时间
        int64_t GetNextMatch(const std::string& cron, int64_t fromTime) const;

        // 获取当前时间戳
        int64_t GetCurrentTimestamp() const;

        // 任务缓存
        std::map<int64_t, std::shared_ptr<ScheduledTask>> tasks_;
        mutable std::mutex mutex_;

        // 回调
        TaskCallback taskCallback_;

        // 初始化标志
        bool initialized_ = false;

        // 上次检查时间
        int64_t lastCheckTime_ = 0;
    };

} // namespace UnnamedBot