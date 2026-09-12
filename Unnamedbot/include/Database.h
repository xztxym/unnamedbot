#pragma once
 
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <functional>

struct sqlite3;

namespace UnnamedBot {

    // 数据库查询结果行，以列名->值映射
    using RowData = std::map<std::string, std::string>;
    using QueryResult = std::vector<RowData>;

    // 数据库封装（单例）
    class Database {
    public:
        static Database& Instance();

        // 初始化数据库，指定路径（默认 "./data/app.db"）
        bool Initialize(const std::wstring& dbPath = L"./data/app.db");

        // 关闭数据库
        void Close();

        // 执行非查询SQL（CREATE, INSERT, UPDATE, DELETE等），返回受影响行数
        int Execute(const std::string& sql, const std::vector<std::string>& params = {});

        // 执行查询SQL，返回结果集
        QueryResult Query(const std::string& sql, const std::vector<std::string>& params = {});

        // 获取最后插入的行ID（用于插入后获取自增ID）
        int64_t GetLastInsertRowId();

        // 事务控制
        bool BeginTransaction();
        bool CommitTransaction();
        bool RollbackTransaction();

        // 准备语句（高级用法，暂不暴露）

    private:
        Database() = default;
        ~Database();

        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // 初始化表结构
        void CreateTables();

        // 检查数据库是否打开
        bool IsOpen() const;

        sqlite3* db_ = nullptr;
        std::recursive_mutex mutex_;
        std::wstring dbPath_;
        bool inTransaction_ = false;
    };

} // namespace UnnamedBot