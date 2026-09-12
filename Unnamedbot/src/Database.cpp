#include "../include/Database.h"
#include "../include/Logger.h"
#include "../include/Utils.h"
#include <sqlite3.h>
#include <sstream>
#include <filesystem>
#include <stdexcept>

namespace UnnamedBot {

    Database& Database::Instance() {
        static Database instance;
        return instance;
    }

    Database::~Database() {
        Close();
    }

    bool Database::Initialize(const std::wstring& dbPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) {
            // 已经初始化，直接返回成功
            return true;
        }

        dbPath_ = dbPath;
        // 确保目录存在
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(dbPath_).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_ERROR("Database", "Failed to create directory for database: " + ec.message());
                return false;
            }
        }

        // 打开数据库
        std::string utf8Path = Utils::PathToUtf8(dbPath_);
        int rc = sqlite3_open(utf8Path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            LOG_ERROR("Database", "Failed to open database: " + std::string(sqlite3_errmsg(db_)));
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        // 启用WAL模式（提高并发性能）
        Execute("PRAGMA journal_mode=WAL;");
        // 启用外键约束
        Execute("PRAGMA foreign_keys=ON;");

        // 创建表
        CreateTables();

        LOG_INFO("Database", "Database initialized successfully: " + Utils::PathToUtf8(dbPath_));
        return true;
    }

    void Database::Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
            LOG_INFO("Database", "Database closed.");
        }
    }

    int Database::Execute(const std::string& sql, const std::vector<std::string>& params) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) {
            LOG_ERROR("Database", "Database not initialized.");
            return -1;
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            LOG_ERROR("Database", "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
            return -1;
        }

        // 绑定参数
        for (size_t i = 0; i < params.size(); ++i) {
            rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            if (rc != SQLITE_OK) {
                LOG_ERROR("Database", "Failed to bind param " + std::to_string(i) + ": " + std::string(sqlite3_errmsg(db_)));
                sqlite3_finalize(stmt);
                return -1;
            }
        }

        rc = sqlite3_step(stmt);
        int changes = 0;
        if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
            changes = sqlite3_changes(db_);
        }
        else {
            LOG_ERROR("Database", "Step failed: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
            sqlite3_finalize(stmt);
            return -1;
        }

        sqlite3_finalize(stmt);
        return changes;
    }

    QueryResult Database::Query(const std::string& sql, const std::vector<std::string>& params) {
        std::lock_guard<std::mutex> lock(mutex_);
        QueryResult result;
        if (!db_) {
            LOG_ERROR("Database", "Database not initialized.");
            return result;
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            LOG_ERROR("Database", "Failed to prepare query: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
            return result;
        }

        // 绑定参数
        for (size_t i = 0; i < params.size(); ++i) {
            rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            if (rc != SQLITE_OK) {
                LOG_ERROR("Database", "Failed to bind param " + std::to_string(i) + ": " + std::string(sqlite3_errmsg(db_)));
                sqlite3_finalize(stmt);
                return result;
            }
        }

        int colCount = sqlite3_column_count(stmt);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            RowData row;
            for (int i = 0; i < colCount; ++i) {
                const char* name = sqlite3_column_name(stmt, i);
                const unsigned char* text = sqlite3_column_text(stmt, i);
                std::string value = text ? reinterpret_cast<const char*>(text) : "";
                row[name] = value;
            }
            result.push_back(row);
        }

        if (rc != SQLITE_DONE) {
            LOG_ERROR("Database", "Query step failed: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
        }

        sqlite3_finalize(stmt);
        return result;
    }

    int64_t Database::GetLastInsertRowId() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return -1;
        return sqlite3_last_insert_rowid(db_);
    }

    bool Database::BeginTransaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return false;
        if (inTransaction_) return true; // 已在事务中
        int rc = sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) {
            inTransaction_ = true;
            return true;
        }
        return false;
    }

    bool Database::CommitTransaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_ || !inTransaction_) return false;
        int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) {
            inTransaction_ = false;
            return true;
        }
        return false;
    }

    bool Database::RollbackTransaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_ || !inTransaction_) return false;
        int rc = sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) {
            inTransaction_ = false;
            return true;
        }
        return false;
    }

    bool Database::IsOpen() const {
        return db_ != nullptr;
    }

    void Database::CreateTables() {
        // 配置表 (key-value)
        Execute(R"(
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
    )");

        // 会话表
        Execute(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            is_active INTEGER DEFAULT 1,
            model_config TEXT,  -- JSON存储模型配置（采样参数等）
            system_prompt TEXT,
            pre_prompt TEXT,
            post_prompt TEXT
        );
    )");

        // 消息表
        Execute(R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER NOT NULL,
            role TEXT NOT NULL,   -- 'user', 'assistant', 'system', 'tool'
            content TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            token_count INTEGER DEFAULT 0,
            extra_data TEXT,      -- JSON存储附件信息、工具调用详情等
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);
        CREATE INDEX IF NOT EXISTS idx_messages_timestamp ON messages(timestamp);
    )");

        // 模型状态表（存储坐标、缩放、透明度、当前模型ID等）
        Execute(R"(
        CREATE TABLE IF NOT EXISTS model_state (
            id INTEGER PRIMARY KEY CHECK (id = 0),  -- 单行
            x INTEGER NOT NULL DEFAULT 100,
            y INTEGER NOT NULL DEFAULT 100,
            scale REAL NOT NULL DEFAULT 1.0,
            opacity REAL NOT NULL DEFAULT 1.0,
            model_id TEXT NOT NULL DEFAULT 'default',
            updated_at INTEGER NOT NULL
        );
    )");

        // 附件表
        Execute(R"(
        CREATE TABLE IF NOT EXISTS attachments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            message_id INTEGER,
            session_id INTEGER NOT NULL,
            filename TEXT NOT NULL,
            filepath TEXT NOT NULL,
            mime_type TEXT,
            size INTEGER NOT NULL,
            uploaded_at INTEGER NOT NULL,
            FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_attachments_session ON attachments(session_id);
    )");

        // 定时任务表
        Execute(R"(
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            cron_expression TEXT NOT NULL,
            action_type TEXT NOT NULL,  -- 'reminder', 'custom'
            action_data TEXT,           -- JSON参数
            enabled INTEGER DEFAULT 1,
            last_run INTEGER DEFAULT 0,
            next_run INTEGER DEFAULT 0,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
    )");

        // 额外：索引优化
        Execute("CREATE INDEX IF NOT EXISTS idx_config_key ON config(key);");
    }

} // namespace UnnamedBot