#pragma once

#include "nvr/common/types.h"
#include <string>
#include <mutex>
#include <memory>

struct sqlite3;

namespace nvr {

class DatabaseManager {
public:
    static DatabaseManager& Instance();

    bool Initialize(const std::string& db_path);
    void Close();

    bool IsOpen() const;
    sqlite3* GetHandle() const;

    std::mutex& GetMutex();

    bool Execute(const std::string& sql);

private:
    DatabaseManager() = default;
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool CreateTables();

    sqlite3* db_{nullptr};
    mutable std::mutex db_mutex_;
    bool initialized_{false};
};

} // namespace nvr
