#include "nvr/storage/database_manager.h"
#include "nvr/common/logger.h"
#include "3rdparty/sqlite3.h"

#include <filesystem>

namespace nvr {

DatabaseManager& DatabaseManager::Instance() {
    static DatabaseManager instance;
    return instance;
}

DatabaseManager::~DatabaseManager() {
    Close();
}

bool DatabaseManager::Initialize(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (initialized_) {
        return true;
    }

    try {
        std::filesystem::path p(db_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to create database directory for " << db_path << ": " << e.what();
        return false;
    }

    int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "Failed to open SQLite database at " << db_path << ": " << sqlite3_errmsg(db_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    // Set WAL mode and robust pragmas for embedded storage
    char* err_msg = nullptr;
    const char* pragma_sql = 
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA busy_timeout = 5000;";

    rc = sqlite3_exec(db_, pragma_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_WARN << "Failed to set pragmas: " << (err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
    }

    if (!CreateTables()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    initialized_ = true;
    LOG_INFO << "SQLite database initialized successfully at " << db_path << " (WAL mode active)";
    return true;
}

bool DatabaseManager::CreateTables() {
    const char* schema_sql =
        "CREATE TABLE IF NOT EXISTS cameras ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  rtsp_url TEXT NOT NULL,"
        "  sub_rtsp_url TEXT,"
        "  enabled INTEGER DEFAULT 1,"
        "  record_mode INTEGER DEFAULT 1,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS recordings ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id INTEGER NOT NULL,"
        "  file_path TEXT UNIQUE NOT NULL,"
        "  start_time_ms INTEGER NOT NULL,"
        "  end_time_ms INTEGER NOT NULL,"
        "  duration_ms INTEGER NOT NULL,"
        "  file_size INTEGER NOT NULL,"
        "  frame_count INTEGER NOT NULL,"
        "  keyframe_count INTEGER NOT NULL,"
        "  is_locked INTEGER DEFAULT 0,"
        "  codec TEXT NOT NULL,"
        "  width INTEGER DEFAULT 1920,"
        "  height INTEGER DEFAULT 1080,"
        "  fps INTEGER DEFAULT 25,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_recordings_channel_time ON recordings (channel_id, start_time_ms, end_time_ms);"
        "CREATE INDEX IF NOT EXISTS idx_recordings_locked ON recordings (is_locked, start_time_ms);"
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id INTEGER NOT NULL,"
        "  event_type TEXT NOT NULL,"
        "  start_time_ms INTEGER NOT NULL,"
        "  end_time_ms INTEGER NOT NULL,"
        "  description TEXT,"
        "  snapshot_path TEXT,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_events_channel_time ON events (channel_id, start_time_ms);";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, schema_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "Failed to create database schema: " << (err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DatabaseManager::Execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!db_) {
        return false;
    }
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "SQL execution failed: " << (err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::IsOpen() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    return initialized_ && (db_ != nullptr);
}

sqlite3* DatabaseManager::GetHandle() const {
    return db_;
}

std::mutex& DatabaseManager::GetMutex() {
    return db_mutex_;
}

void DatabaseManager::Close() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    initialized_ = false;
}

} // namespace nvr
