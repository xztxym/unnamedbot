#include "../include/TaskScheduler.h"
#include "../include/Database.h"
#include "../include/Logger.h"
#include <sstream>
#include <regex>
#include <ctime>
#include <iomanip>
#include <chrono>

namespace UnnamedBot {

    TaskScheduler& TaskScheduler::Instance() {
        static TaskScheduler instance;
        return instance;
    }

    void TaskScheduler::Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            return;
        }

        // 加载任务
        LoadTasks();

        // 计算下次执行时间
        for (auto& pair : tasks_) {
            if (pair.second->enabled) {
                pair.second->nextRun = CalculateNextRun(pair.second->cronExpression, GetCurrentTimestamp());
            }
        }

        lastCheckTime_ = GetCurrentTimestamp();
        initialized_ = true;
        LOG_INFO("TaskScheduler", "Initialized with " + std::to_string(tasks_.size()) + " tasks.");
    }

    void TaskScheduler::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.clear();
        initialized_ = false;
        LOG_INFO("TaskScheduler", "Shut down.");
    }

    int64_t TaskScheduler::AddTask(
        const std::string& name,
        const std::string& cronExpression,
        const std::string& actionType,
        const std::string& actionData
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        int64_t now = GetCurrentTimestamp();

        ScheduledTask task;
        task.name = name;
        task.cronExpression = cronExpression;
        task.actionType = actionType;
        task.actionData = actionData.empty() ? "{}" : actionData;
        task.enabled = true;
        task.createdAt = now;
        task.updatedAt = now;
        task.lastRun = 0;
        task.nextRun = CalculateNextRun(cronExpression, now);

        if (!SaveTask(task)) {
            LOG_ERROR("TaskScheduler", "Failed to save task.");
            return -1;
        }

        tasks_[task.id] = std::make_shared<ScheduledTask>(task);
        LOG_INFO("TaskScheduler", "Added task: " + name + " (ID: " + std::to_string(task.id) + ")");
        return task.id;
    }

    bool TaskScheduler::DeleteTask(int64_t taskId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            LOG_WARN("TaskScheduler", "Task not found: " + std::to_string(taskId));
            return false;
        }

        Database::Instance().Execute(
            "DELETE FROM tasks WHERE id = ?;",
            { std::to_string(taskId) }
        );

        tasks_.erase(it);
        LOG_INFO("TaskScheduler", "Deleted task: " + std::to_string(taskId));
        return true;
    }

    bool TaskScheduler::EnableTask(int64_t taskId, bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            LOG_WARN("TaskScheduler", "Task not found: " + std::to_string(taskId));
            return false;
        }

        it->second->enabled = enabled;
        it->second->updatedAt = GetCurrentTimestamp();

        if (enabled) {
            it->second->nextRun = CalculateNextRun(it->second->cronExpression, GetCurrentTimestamp());
        }

        Database::Instance().Execute(
            "UPDATE tasks SET enabled = ?, next_run = ?, updated_at = ? WHERE id = ?;",
            {
                enabled ? "1" : "0",
                std::to_string(it->second->nextRun),
                std::to_string(it->second->updatedAt),
                std::to_string(taskId)
            }
        );

        LOG_DEBUG("TaskScheduler", (enabled ? "Enabled" : "Disabled") + " task: " + std::to_string(taskId));
        return true;
    }

    bool TaskScheduler::UpdateTask(int64_t taskId, const ScheduledTask& task) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            LOG_WARN("TaskScheduler", "Task not found: " + std::to_string(taskId));
            return false;
        }

        *it->second = task;
        it->second->updatedAt = GetCurrentTimestamp();
        it->second->nextRun = CalculateNextRun(task.cronExpression, GetCurrentTimestamp());

        return SaveTask(*it->second);
    }

    std::shared_ptr<ScheduledTask> TaskScheduler::GetTask(int64_t taskId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tasks_.find(taskId);
        if (it != tasks_.end()) {
            return it->second;
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<ScheduledTask>> TaskScheduler::GetAllTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::shared_ptr<ScheduledTask>> result;
        for (const auto& pair : tasks_) {
            result.push_back(pair.second);
        }
        return result;
    }

    std::vector<std::shared_ptr<ScheduledTask>> TaskScheduler::GetEnabledTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::shared_ptr<ScheduledTask>> result;
        for (const auto& pair : tasks_) {
            if (pair.second->enabled) {
                result.push_back(pair.second);
            }
        }
        return result;
    }

    bool TaskScheduler::ExecuteTask(int64_t taskId) {
        auto task = GetTask(taskId);
        if (!task) {
            LOG_ERROR("TaskScheduler", "Task not found: " + std::to_string(taskId));
            return false;
        }

        if (!task->enabled) {
            LOG_WARN("TaskScheduler", "Task is disabled: " + std::to_string(taskId));
            return false;
        }

        LOG_INFO("TaskScheduler", "Executing task: " + task->name);

        if (taskCallback_) {
            taskCallback_(*task);
        }

        // 更新上次运行时间
        task->lastRun = GetCurrentTimestamp();
        task->nextRun = CalculateNextRun(task->cronExpression, task->lastRun);

        Database::Instance().Execute(
            "UPDATE tasks SET last_run = ?, next_run = ? WHERE id = ?;",
            {
                std::to_string(task->lastRun),
                std::to_string(task->nextRun),
                std::to_string(taskId)
            }
        );

        return true;
    }

    void TaskScheduler::Update() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        int64_t now = GetCurrentTimestamp();

        // 每分钟检查一次
        if (now - lastCheckTime_ < 60) {
            return;
        }
        lastCheckTime_ = now;

        CheckAndExecuteTasks();
    }

    void TaskScheduler::OnTaskExecute(TaskCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        taskCallback_ = callback;
    }

    int64_t TaskScheduler::CalculateNextRun(const std::string& cronExpression, int64_t fromTime) const {
        if (fromTime == 0) {
            fromTime = GetCurrentTimestamp();
        }

        return GetNextMatch(cronExpression, fromTime);
    }

    // ============ 私有方法 ============

    void TaskScheduler::LoadTasks() {
        auto rows = Database::Instance().Query(
            "SELECT id, name, cron_expression, action_type, action_data, enabled, "
            "last_run, next_run, created_at, updated_at FROM tasks;"
        );

        tasks_.clear();
        for (const auto& row : rows) {
            auto task = std::make_shared<ScheduledTask>();
            task->id = std::stoll(row.at("id"));
            task->name = row.at("name");
            task->cronExpression = row.at("cron_expression");
            task->actionType = row.at("action_type");
            task->actionData = row.at("action_data");
            task->enabled = row.at("enabled") == "1";
            task->lastRun = std::stoll(row.at("last_run"));
            task->nextRun = std::stoll(row.at("next_run"));
            task->createdAt = std::stoll(row.at("created_at"));
            task->updatedAt = std::stoll(row.at("updated_at"));

            tasks_[task->id] = task;
        }
    }

    bool TaskScheduler::SaveTask(const ScheduledTask& task) {
        Database::Instance().Execute(
            "INSERT OR REPLACE INTO tasks (id, name, cron_expression, action_type, action_data, "
            "enabled, last_run, next_run, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            {
                std::to_string(task.id),
                task.name,
                task.cronExpression,
                task.actionType,
                task.actionData,
                task.enabled ? "1" : "0",
                std::to_string(task.lastRun),
                std::to_string(task.nextRun),
                std::to_string(task.createdAt),
                std::to_string(task.updatedAt)
            }
        );
        return true;
    }

    void TaskScheduler::CheckAndExecuteTasks() {
        int64_t now = GetCurrentTimestamp();

        for (auto& pair : tasks_) {
            auto& task = pair.second;
            if (!task->enabled) continue;
            if (task->nextRun == 0) {
                task->nextRun = CalculateNextRun(task->cronExpression, now);
                continue;
            }
            if (task->nextRun <= now) {
                // 执行任务
                ExecuteTask(task->id);
            }
        }
    }

    int64_t TaskScheduler::GetNextMatch(const std::string& cron, int64_t fromTime) const {
        // 简化实现：每5分钟检查一次
        // 实际应实现完整的Cron解析器
        // 这里使用简化版本：支持分、时、日、月、周
        int minute, hour, day, month, dayOfWeek;
        if (!ParseCron(cron, minute, hour, day, month, dayOfWeek)) {
            // 解析失败，返回1分钟后
            return fromTime + 60;
        }

        // 从fromTime开始，找到下一个匹配的时间
        time_t time = fromTime;
        struct tm tm_time;
        localtime_s(&tm_time, &time);

        // 最多搜索一年
        for (int i = 0; i < 365 * 24 * 60; i++) {
            time_t current = fromTime + i * 60;
            localtime_s(&tm_time, &current);

            // 检查分钟
            if (minute != -1 && tm_time.tm_min != minute) continue;

            // 检查小时
            if (hour != -1 && tm_time.tm_hour != hour) continue;

            // 检查日
            if (day != -1 && tm_time.tm_mday != day) continue;

            // 检查月
            if (month != -1 && (tm_time.tm_mon + 1) != month) continue;

            // 检查星期（0=Sunday）
            if (dayOfWeek != -1 && tm_time.tm_wday != dayOfWeek) continue;

            // 匹配成功
            return current;
        }

        // 未找到，返回1小时后
        return fromTime + 3600;
    }

    bool TaskScheduler::ParseCron(const std::string& cron, int& minute, int& hour, int& day, int& month, int& dayOfWeek) {
        // 解析Cron表达式：分 时 日 月 周
        std::istringstream iss(cron);
        std::vector<std::string> parts;
        std::string part;

        while (std::getline(iss, part, ' ')) {
            if (!part.empty()) {
                parts.push_back(part);
            }
        }

        if (parts.size() < 5) {
            return false;
        }

        // 简化解析：支持 * 和数字
        auto parsePart = [](const std::string& str, int& value) -> bool {
            if (str == "*") {
                value = -1;
                return true;
            }
            try {
                value = std::stoi(str);
                return true;
            }
            catch (...) {
                return false;
            }
            };

        if (!parsePart(parts[0], minute)) return false;
        if (!parsePart(parts[1], hour)) return false;
        if (!parsePart(parts[2], day)) return false;
        if (!parsePart(parts[3], month)) return false;
        if (!parsePart(parts[4], dayOfWeek)) return false;

        return true;
    }

    int64_t TaskScheduler::GetCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }

} // namespace UnnamedBot