#include "nvr/database/database_manager.h"
#include "3rdparty/sqlite3.h"
#include "nvr/common/crypto_utils.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/config_loader.h"

#include <filesystem>
#include <iostream>
#include <sstream>

namespace nvr {

namespace {

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK) {
            LOG_ERROR << "[DB] Prepare failed: " << sqlite3_errmsg(db) << " | SQL: " << sql;
            stmt_ = nullptr;
        }
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    sqlite3_stmt* get() const { return stmt_; }
    bool isValid() const { return stmt_ != nullptr; }

    int step() { return stmt_ ? sqlite3_step(stmt_) : SQLITE_ERROR; }
    void reset() { if (stmt_) sqlite3_reset(stmt_); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

int64_t CurrentTimeMs() {
    return time_utils::WallTimeMs();
}

} // namespace

DatabaseManager& DatabaseManager::Instance() {
    static DatabaseManager instance;
    return instance;
}

DatabaseManager::~DatabaseManager() {
    Close();
}

bool DatabaseManager::Initialize(const std::string& db_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) return true; // Already open

    db_path_ = db_path.empty() ? "config/nvr.db" : db_path;

    std::error_code ec;
    std::filesystem::path p(db_path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    vault_key_ = crypto::GetOrCreateVaultKey();

    int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "[DB] Failed to open SQLite database at " << db_path_ << ": "
                  << (db_ ? sqlite3_errmsg(db_) : "unknown error");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }

    // Configure resilient embedded PRAGMAs
    char* err_msg = nullptr;
    const char* pragma_sql =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA busy_timeout = 5000;";

    if (sqlite3_exec(db_, pragma_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        LOG_WARN << "[DB] Pragma configuration warning: " << (err_msg ? err_msg : "");
        sqlite3_free(err_msg);
    }

    if (!ExecuteSchema()) {
        LOG_ERROR << "[DB] Schema initialization failed";
        return false;
    }

    BootstrapDefaults();

    LOG_INFO << "[DB] SQLite Database initialized successfully in WAL mode: " << db_path_;
    return true;
}

void DatabaseManager::Close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        LOG_INFO << "[DB] SQLite Database closed";
    }
}

bool DatabaseManager::IsOpen() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return db_ != nullptr;
}

bool DatabaseManager::ExecuteSchema() {
    const char* schema_sql = R"SQL(
    CREATE TABLE IF NOT EXISTS nvr_users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL,
        salt TEXT NOT NULL,
        role TEXT NOT NULL DEFAULT 'VIEWER',
        is_active INTEGER NOT NULL DEFAULT 1,
        failed_attempts INTEGER NOT NULL DEFAULT 0,
        locked_until INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS nvr_sessions (
        token TEXT PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES nvr_users(id) ON DELETE CASCADE,
        role TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_sessions_expires ON nvr_sessions(expires_at);

    CREATE TABLE IF NOT EXISTS nvr_cameras (
        channel_id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        ip_address TEXT NOT NULL,
        onvif_xaddr TEXT NOT NULL,
        onvif_username TEXT NOT NULL,
        onvif_password_enc TEXT NOT NULL,
        rtsp_main_url TEXT NOT NULL,
        rtsp_sub_url TEXT NOT NULL,
        codec TEXT NOT NULL DEFAULT 'H264',
        resolution TEXT NOT NULL DEFAULT '1080p',
        fps INTEGER NOT NULL DEFAULT 25,
        bitrate_kbps INTEGER NOT NULL DEFAULT 4096,
        record_mode INTEGER NOT NULL DEFAULT 1,
        pre_roll_sec INTEGER NOT NULL DEFAULT 5,
        post_roll_sec INTEGER NOT NULL DEFAULT 10,
        schedule_json TEXT NOT NULL DEFAULT '{}',
        ai_model TEXT NOT NULL DEFAULT 'None',
        enabled INTEGER NOT NULL DEFAULT 1,
        updated_at INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS nvr_recordings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id INTEGER NOT NULL REFERENCES nvr_cameras(channel_id),
        start_time_ms INTEGER NOT NULL,
        end_time_ms INTEGER NOT NULL,
        duration_sec INTEGER NOT NULL,
        file_path TEXT NOT NULL,
        file_size_bytes INTEGER NOT NULL,
        codec TEXT NOT NULL,
        trigger_type TEXT NOT NULL DEFAULT 'continuous',
        is_locked INTEGER NOT NULL DEFAULT 0,
        created_at_ms INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_recordings_channel_time 
        ON nvr_recordings(channel_id, start_time_ms, end_time_ms);
    CREATE INDEX IF NOT EXISTS idx_recordings_locked ON nvr_recordings(is_locked);
    CREATE UNIQUE INDEX IF NOT EXISTS idx_recordings_path ON nvr_recordings(file_path);

    CREATE TABLE IF NOT EXISTS nvr_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id INTEGER NOT NULL REFERENCES nvr_cameras(channel_id),
        event_type TEXT NOT NULL,
        severity TEXT NOT NULL DEFAULT 'info',
        start_time_ms INTEGER NOT NULL,
        end_time_ms INTEGER NOT NULL,
        confidence REAL NOT NULL DEFAULT 0.0,
        details_json TEXT NOT NULL DEFAULT '{}',
        thumbnail_path TEXT NOT NULL DEFAULT '',
        has_clip INTEGER NOT NULL DEFAULT 0,
        is_acknowledged INTEGER NOT NULL DEFAULT 0,
        created_at_ms INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_events_lookup 
        ON nvr_events(channel_id, event_type, start_time_ms);

    CREATE TABLE IF NOT EXISTS nvr_ai_configs (
        channel_id INTEGER NOT NULL REFERENCES nvr_cameras(channel_id),
        model_type TEXT NOT NULL,
        sensitivity INTEGER NOT NULL DEFAULT 75,
        zones_json TEXT NOT NULL DEFAULT '[]',
        schedule_json TEXT NOT NULL DEFAULT '{}',
        enabled INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (channel_id, model_type)
    );

    CREATE TABLE IF NOT EXISTS nvr_audit_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp_ms INTEGER NOT NULL,
        actor_username TEXT NOT NULL,
        actor_role TEXT NOT NULL,
        device_ip TEXT NOT NULL,
        action TEXT NOT NULL,
        section TEXT NOT NULL,
        summary TEXT NOT NULL,
        details_json TEXT NOT NULL DEFAULT '{}',
        status TEXT NOT NULL DEFAULT 'SUCCESS'
    );
    CREATE INDEX IF NOT EXISTS idx_audit_time ON nvr_audit_log(timestamp_ms DESC);

    CREATE TABLE IF NOT EXISTS nvr_system_settings (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL,
        updated_at INTEGER NOT NULL
    );
    )SQL";

    char* err_msg = nullptr;
    if (sqlite3_exec(db_, schema_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        LOG_ERROR << "[DB] Failed to execute schema: " << (err_msg ? err_msg : "");
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

void DatabaseManager::BootstrapDefaults() {
    // 1. Check if users table is empty; if so, create initial default admin
    Statement stmt_count_user(db_, "SELECT COUNT(*) FROM nvr_users;");
    if (stmt_count_user.isValid() && stmt_count_user.step() == SQLITE_ROW) {
        if (sqlite3_column_int(stmt_count_user.get(), 0) == 0) {
            std::string err;
            if (CreateUser("admin", "Admin@123", "ADMIN", &err)) {
                LOG_INFO << "[DB] Bootstrapped initial administrator account: 'admin'";
                AppendAuditLog("system", "ADMIN", "127.0.0.1", "system.bootstrap", "security",
                               "Initialized factory administrator account", "{}");
            }
        }
    }

    // 2. Check if cameras table is empty; if so, seed from config/nvr_config.json
    Statement stmt_count_cam(db_, "SELECT COUNT(*) FROM nvr_cameras;");
    if (stmt_count_cam.isValid() && stmt_count_cam.step() == SQLITE_ROW) {
        if (sqlite3_column_int(stmt_count_cam.get(), 0) == 0) {
            NvrConfig cfg = LoadNvrConfig("config/nvr_config.json");
            for (const auto& cam : cfg.cameras) {
                CameraRecord rec;
                rec.channel_id = cam.id;
                rec.name = cam.name;
                rec.onvif_xaddr = cam.onvif_xaddr;
                rec.onvif_username = cam.onvif_username;
                rec.onvif_password = cam.onvif_password;
                rec.rtsp_main_url = cam.rtsp_url;
                rec.rtsp_sub_url = cam.sub_rtsp_url;
                rec.record_mode = static_cast<int>(cam.record_mode);
                rec.enabled = cam.enabled;

                // Extract IP from ONVIF or RTSP
                std::string ip = "127.0.0.1";
                auto at_pos = cam.rtsp_url.find('@');
                if (at_pos != std::string::npos) {
                    auto colon_pos = cam.rtsp_url.find(':', at_pos);
                    if (colon_pos != std::string::npos) {
                        ip = cam.rtsp_url.substr(at_pos + 1, colon_pos - (at_pos + 1));
                    }
                }
                rec.ip_address = ip;

                UpsertCamera(rec);
            }
            LOG_INFO << "[DB] Migrated " << cfg.cameras.size() << " camera channels from config file to database";
        }
    }

    // 3. Seed default system settings if empty
    Statement stmt_count_settings(db_, "SELECT COUNT(*) FROM nvr_system_settings;");
    if (stmt_count_settings.isValid() && stmt_count_settings.step() == SQLITE_ROW) {
        if (sqlite3_column_int(stmt_count_settings.get(), 0) == 0) {
            SetSetting("device_name", "NVR_Main");
            SetSetting("max_retention_days", "14");
            SetSetting("min_free_space_mb", "512");
            SetSetting("segment_duration_seconds", "60");
            SetSetting("grid_layout", "4");
            SetSetting("voice_assistant_enabled", "1");
            SetSetting("voice_wake_word", "hey mantra");
        }
    }
}

// ==========================================
// User & RBAC Management
// ==========================================

bool DatabaseManager::CreateUser(const std::string& username, const std::string& password,
                                 const std::string& role, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    if (username.empty() || password.empty()) {
        if (error) *error = "Username and password cannot be empty";
        return false;
    }
    if (password.length() < 8) {
        if (error) *error = "Password must be at least 8 characters";
        return false;
    }

    std::string salt = crypto::GenerateRandomHex(16);
    std::string hash = crypto::HashPasswordPBKDF2(password, salt);
    int64_t now = CurrentTimeMs();

    std::string clean_role = "VIEWER";
    if (role == "ADMIN" || role == "admin") clean_role = "ADMIN";
    else if (role == "OPERATOR" || role == "operator") clean_role = "OPERATOR";

    Statement stmt(db_, "INSERT INTO nvr_users (username, password_hash, salt, role, is_active, failed_attempts, locked_until, created_at, updated_at) "
                        "VALUES (?, ?, ?, ?, 1, 0, 0, ?, ?);");
    if (!stmt.isValid()) return false;

    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, clean_role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 5, now);
    sqlite3_bind_int64(stmt.get(), 6, now);

    if (stmt.step() != SQLITE_DONE) {
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool DatabaseManager::VerifyUserCredentials(const std::string& username, const std::string& password,
                                            UserRecord& out_user, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "SELECT id, username, password_hash, salt, role, is_active, failed_attempts, locked_until, created_at, updated_at "
                        "FROM nvr_users WHERE username = ? COLLATE NOCASE;");
    if (!stmt.isValid()) return false;

    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    if (stmt.step() != SQLITE_ROW) {
        if (error) *error = "Invalid username or password";
        return false;
    }

    out_user.id = sqlite3_column_int(stmt.get(), 0);
    out_user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    out_user.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    out_user.salt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    out_user.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    out_user.is_active = sqlite3_column_int(stmt.get(), 5) != 0;
    out_user.failed_attempts = sqlite3_column_int(stmt.get(), 6);
    out_user.locked_until = sqlite3_column_int64(stmt.get(), 7);
    out_user.created_at = sqlite3_column_int64(stmt.get(), 8);
    out_user.updated_at = sqlite3_column_int64(stmt.get(), 9);

    if (!out_user.is_active) {
        if (error) *error = "Account is disabled. Contact system administrator.";
        return false;
    }

    int64_t now = CurrentTimeMs();
    if (out_user.locked_until > now) {
        int remain_min = static_cast<int>((out_user.locked_until - now) / 60000) + 1;
        if (error) *error = "Account temporarily locked. Retry in " + std::to_string(remain_min) + " minutes.";
        return false;
    }

    bool matched = crypto::VerifyPassword(password, out_user.salt, out_user.password_hash);
    if (!matched) {
        int new_failures = out_user.failed_attempts + 1;
        int64_t lock_time = 0;
        if (new_failures >= 5) {
            lock_time = now + (15 * 60 * 1000); // 15-minute lockout
            LOG_WARN << "[DB] User '" << username << "' locked out after 5 consecutive failed attempts";
        }
        Statement stmt_fail(db_, "UPDATE nvr_users SET failed_attempts = ?, locked_until = ?, updated_at = ? WHERE id = ?;");
        if (stmt_fail.isValid()) {
            sqlite3_bind_int(stmt_fail.get(), 1, new_failures);
            sqlite3_bind_int64(stmt_fail.get(), 2, lock_time);
            sqlite3_bind_int64(stmt_fail.get(), 3, now);
            sqlite3_bind_int(stmt_fail.get(), 4, out_user.id);
            stmt_fail.step();
        }
        if (error) *error = "Invalid username or password";
        return false;
    }

    // Reset failed attempts on success
    Statement stmt_reset(db_, "UPDATE nvr_users SET failed_attempts = 0, locked_until = 0, updated_at = ? WHERE id = ?;");
    if (stmt_reset.isValid()) {
        sqlite3_bind_int64(stmt_reset.get(), 1, now);
        sqlite3_bind_int(stmt_reset.get(), 2, out_user.id);
        stmt_reset.step();
    }

    return true;
}

bool DatabaseManager::ChangeUserPassword(int user_id, const std::string& old_password,
                                         const std::string& new_password, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    if (new_password.length() < 8) {
        if (error) *error = "New password must be at least 8 characters";
        return false;
    }

    Statement stmt_find(db_, "SELECT password_hash, salt FROM nvr_users WHERE id = ?;");
    if (!stmt_find.isValid()) return false;
    sqlite3_bind_int(stmt_find.get(), 1, user_id);

    if (stmt_find.step() != SQLITE_ROW) {
        if (error) *error = "User not found";
        return false;
    }

    std::string stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find.get(), 0));
    std::string stored_salt = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find.get(), 1));

    if (!crypto::VerifyPassword(old_password, stored_salt, stored_hash)) {
        if (error) *error = "Current password is incorrect";
        return false;
    }

    std::string new_salt = crypto::GenerateRandomHex(16);
    std::string new_hash = crypto::HashPasswordPBKDF2(new_password, new_salt);
    int64_t now = CurrentTimeMs();

    Statement stmt_upd(db_, "UPDATE nvr_users SET password_hash = ?, salt = ?, updated_at = ? WHERE id = ?;");
    if (!stmt_upd.isValid()) return false;

    sqlite3_bind_text(stmt_upd.get(), 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upd.get(), 2, new_salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upd.get(), 3, now);
    sqlite3_bind_int(stmt_upd.get(), 4, user_id);

    return stmt_upd.step() == SQLITE_DONE;
}

bool DatabaseManager::AdminResetPassword(int user_id, const std::string& new_password, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    if (new_password.length() < 8) {
        if (error) *error = "New password must be at least 8 characters";
        return false;
    }

    std::string new_salt = crypto::GenerateRandomHex(16);
    std::string new_hash = crypto::HashPasswordPBKDF2(new_password, new_salt);
    int64_t now = CurrentTimeMs();

    Statement stmt(db_, "UPDATE nvr_users SET password_hash = ?, salt = ?, failed_attempts = 0, locked_until = 0, updated_at = ? WHERE id = ?;");
    if (!stmt.isValid()) return false;

    sqlite3_bind_text(stmt.get(), 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, new_salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, now);
    sqlite3_bind_int(stmt.get(), 4, user_id);

    return stmt.step() == SQLITE_DONE;
}

bool DatabaseManager::DeleteUser(int user_id, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    // Prevent deleting the primary admin account
    Statement stmt_check(db_, "SELECT username, role FROM nvr_users WHERE id = ?;");
    if (stmt_check.isValid()) {
        sqlite3_bind_int(stmt_check.get(), 1, user_id);
        if (stmt_check.step() == SQLITE_ROW) {
            std::string uname = reinterpret_cast<const char*>(sqlite3_column_text(stmt_check.get(), 0));
            std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt_check.get(), 1));
            if (uname == "admin") {
                if (error) *error = "Cannot delete the primary 'admin' account";
                return false;
            }
        }
    }

    Statement stmt(db_, "DELETE FROM nvr_users WHERE id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, user_id);

    return stmt.step() == SQLITE_DONE;
}

std::vector<UserRecord> DatabaseManager::ListUsers() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<UserRecord> users;
    if (!db_) return users;

    Statement stmt(db_, "SELECT id, username, role, is_active, failed_attempts, locked_until, created_at, updated_at "
                        "FROM nvr_users ORDER BY id ASC;");
    if (!stmt.isValid()) return users;

    while (stmt.step() == SQLITE_ROW) {
        UserRecord u;
        u.id = sqlite3_column_int(stmt.get(), 0);
        u.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        u.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        u.is_active = sqlite3_column_int(stmt.get(), 3) != 0;
        u.failed_attempts = sqlite3_column_int(stmt.get(), 4);
        u.locked_until = sqlite3_column_int64(stmt.get(), 5);
        u.created_at = sqlite3_column_int64(stmt.get(), 6);
        u.updated_at = sqlite3_column_int64(stmt.get(), 7);
        // Sensitive password_hash and salt remain empty
        users.push_back(u);
    }
    return users;
}

bool DatabaseManager::GetUserById(int user_id, UserRecord& out_user) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "SELECT id, username, role, is_active, failed_attempts, locked_until, created_at, updated_at "
                        "FROM nvr_users WHERE id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, user_id);

    if (stmt.step() != SQLITE_ROW) return false;

    out_user.id = sqlite3_column_int(stmt.get(), 0);
    out_user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    out_user.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    out_user.is_active = sqlite3_column_int(stmt.get(), 3) != 0;
    out_user.failed_attempts = sqlite3_column_int(stmt.get(), 4);
    out_user.locked_until = sqlite3_column_int64(stmt.get(), 5);
    out_user.created_at = sqlite3_column_int64(stmt.get(), 6);
    out_user.updated_at = sqlite3_column_int64(stmt.get(), 7);
    return true;
}

bool DatabaseManager::GetUserByUsername(const std::string& username, UserRecord& out_user) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "SELECT id, username, role, is_active, failed_attempts, locked_until, created_at, updated_at "
                        "FROM nvr_users WHERE username = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    if (stmt.step() != SQLITE_ROW) return false;

    out_user.id = sqlite3_column_int(stmt.get(), 0);
    out_user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    out_user.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    out_user.is_active = sqlite3_column_int(stmt.get(), 3) != 0;
    out_user.failed_attempts = sqlite3_column_int(stmt.get(), 4);
    out_user.locked_until = sqlite3_column_int64(stmt.get(), 5);
    out_user.created_at = sqlite3_column_int64(stmt.get(), 6);
    out_user.updated_at = sqlite3_column_int64(stmt.get(), 7);
    return true;
}

// ==========================================
// Session Tokens
// ==========================================

std::string DatabaseManager::CreateSession(int user_id, int duration_sec) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return "";

    Statement stmt_user(db_, "SELECT role FROM nvr_users WHERE id = ? AND is_active = 1;");
    if (!stmt_user.isValid()) return "";
    sqlite3_bind_int(stmt_user.get(), 1, user_id);

    if (stmt_user.step() != SQLITE_ROW) return "";
    std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt_user.get(), 0));

    std::string token = crypto::GenerateSessionToken();
    int64_t now = CurrentTimeMs();
    int64_t expires = now + (static_cast<int64_t>(duration_sec) * 1000);

    Statement stmt(db_, "INSERT INTO nvr_sessions (token, user_id, role, created_at, expires_at) VALUES (?, ?, ?, ?, ?);");
    if (!stmt.isValid()) return "";

    sqlite3_bind_text(stmt.get(), 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, user_id);
    sqlite3_bind_text(stmt.get(), 3, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, now);
    sqlite3_bind_int64(stmt.get(), 5, expires);

    if (stmt.step() != SQLITE_DONE) return "";
    return token;
}

bool DatabaseManager::ValidateSession(const std::string& token, SessionRecord& out_session) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || token.empty()) return false;

    Statement stmt(db_, "SELECT s.token, s.user_id, u.username, s.role, s.created_at, s.expires_at "
                        "FROM nvr_sessions s "
                        "JOIN nvr_users u ON s.user_id = u.id "
                        "WHERE s.token = ? AND u.is_active = 1;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_text(stmt.get(), 1, token.c_str(), -1, SQLITE_TRANSIENT);

    if (stmt.step() != SQLITE_ROW) return false;

    int64_t expires = sqlite3_column_int64(stmt.get(), 5);
    if (expires < CurrentTimeMs()) {
        // Expired
        return false;
    }

    out_session.token = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    out_session.user_id = sqlite3_column_int(stmt.get(), 1);
    out_session.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    out_session.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    out_session.created_at = sqlite3_column_int64(stmt.get(), 4);
    out_session.expires_at = expires;
    return true;
}

bool DatabaseManager::RevokeSession(const std::string& token) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || token.empty()) return false;

    Statement stmt(db_, "DELETE FROM nvr_sessions WHERE token = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_text(stmt.get(), 1, token.c_str(), -1, SQLITE_TRANSIENT);

    return stmt.step() == SQLITE_DONE;
}

void DatabaseManager::CleanupExpiredSessions() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return;

    Statement stmt(db_, "DELETE FROM nvr_sessions WHERE expires_at < ?;");
    if (!stmt.isValid()) return;
    sqlite3_bind_int64(stmt.get(), 1, CurrentTimeMs());
    stmt.step();
}

// ==========================================
// Camera Inventory
// ==========================================

bool DatabaseManager::UpsertCamera(const CameraRecord& cam) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string enc_pass = crypto::EncryptAesGcm(cam.onvif_password, vault_key_);
    int64_t now = CurrentTimeMs();

    const char* sql = R"SQL(
        INSERT INTO nvr_cameras (channel_id, name, ip_address, onvif_xaddr, onvif_username, onvif_password_enc,
                                 rtsp_main_url, rtsp_sub_url, codec, resolution, fps, bitrate_kbps, record_mode,
                                 pre_roll_sec, post_roll_sec, schedule_json, ai_model, enabled, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(channel_id) DO UPDATE SET
            name = excluded.name,
            ip_address = excluded.ip_address,
            onvif_xaddr = excluded.onvif_xaddr,
            onvif_username = excluded.onvif_username,
            onvif_password_enc = excluded.onvif_password_enc,
            rtsp_main_url = excluded.rtsp_main_url,
            rtsp_sub_url = excluded.rtsp_sub_url,
            codec = excluded.codec,
            resolution = excluded.resolution,
            fps = excluded.fps,
            bitrate_kbps = excluded.bitrate_kbps,
            record_mode = excluded.record_mode,
            pre_roll_sec = excluded.pre_roll_sec,
            post_roll_sec = excluded.post_roll_sec,
            schedule_json = excluded.schedule_json,
            ai_model = excluded.ai_model,
            enabled = excluded.enabled,
            updated_at = excluded.updated_at;
    )SQL";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;

    sqlite3_bind_int(stmt.get(), 1, cam.channel_id);
    sqlite3_bind_text(stmt.get(), 2, cam.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, cam.ip_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, cam.onvif_xaddr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, cam.onvif_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, enc_pass.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, cam.rtsp_main_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, cam.rtsp_sub_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, cam.codec.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 10, cam.resolution.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 11, cam.fps);
    sqlite3_bind_int(stmt.get(), 12, cam.bitrate_kbps);
    sqlite3_bind_int(stmt.get(), 13, cam.record_mode);
    sqlite3_bind_int(stmt.get(), 14, cam.pre_roll_sec);
    sqlite3_bind_int(stmt.get(), 15, cam.post_roll_sec);
    sqlite3_bind_text(stmt.get(), 16, cam.schedule_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 17, cam.ai_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 18, cam.enabled ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 19, now);

    return stmt.step() == SQLITE_DONE;
}

bool DatabaseManager::DeleteCamera(int channel_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "DELETE FROM nvr_cameras WHERE channel_id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, channel_id);

    return stmt.step() == SQLITE_DONE;
}

std::vector<CameraRecord> DatabaseManager::GetCameras() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<CameraRecord> cams;
    if (!db_) return cams;

    const char* sql = "SELECT channel_id, name, ip_address, onvif_xaddr, onvif_username, onvif_password_enc, "
                      "rtsp_main_url, rtsp_sub_url, codec, resolution, fps, bitrate_kbps, record_mode, "
                      "pre_roll_sec, post_roll_sec, schedule_json, ai_model, enabled, updated_at "
                      "FROM nvr_cameras ORDER BY channel_id ASC;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return cams;

    while (stmt.step() == SQLITE_ROW) {
        CameraRecord c;
        c.channel_id = sqlite3_column_int(stmt.get(), 0);
        c.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        c.ip_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        c.onvif_xaddr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        c.onvif_username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        std::string enc_pass = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        c.onvif_password = crypto::DecryptAesGcm(enc_pass, vault_key_);
        c.rtsp_main_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        c.rtsp_sub_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        c.codec = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        c.resolution = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
        c.fps = sqlite3_column_int(stmt.get(), 10);
        c.bitrate_kbps = sqlite3_column_int(stmt.get(), 11);
        c.record_mode = sqlite3_column_int(stmt.get(), 12);
        c.pre_roll_sec = sqlite3_column_int(stmt.get(), 13);
        c.post_roll_sec = sqlite3_column_int(stmt.get(), 14);
        c.schedule_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 15));
        c.ai_model = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 16));
        c.enabled = sqlite3_column_int(stmt.get(), 17) != 0;
        c.updated_at = sqlite3_column_int64(stmt.get(), 18);
        cams.push_back(c);
    }
    return cams;
}

bool DatabaseManager::GetCamera(int channel_id, CameraRecord& out_cam) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT channel_id, name, ip_address, onvif_xaddr, onvif_username, onvif_password_enc, "
                      "rtsp_main_url, rtsp_sub_url, codec, resolution, fps, bitrate_kbps, record_mode, "
                      "pre_roll_sec, post_roll_sec, schedule_json, ai_model, enabled, updated_at "
                      "FROM nvr_cameras WHERE channel_id = ?;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, channel_id);

    if (stmt.step() != SQLITE_ROW) return false;

    out_cam.channel_id = sqlite3_column_int(stmt.get(), 0);
    out_cam.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    out_cam.ip_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    out_cam.onvif_xaddr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    out_cam.onvif_username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    std::string enc_pass = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
    out_cam.onvif_password = crypto::DecryptAesGcm(enc_pass, vault_key_);
    out_cam.rtsp_main_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    out_cam.rtsp_sub_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    out_cam.codec = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
    out_cam.resolution = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
    out_cam.fps = sqlite3_column_int(stmt.get(), 10);
    out_cam.bitrate_kbps = sqlite3_column_int(stmt.get(), 11);
    out_cam.record_mode = sqlite3_column_int(stmt.get(), 12);
    out_cam.pre_roll_sec = sqlite3_column_int(stmt.get(), 13);
    out_cam.post_roll_sec = sqlite3_column_int(stmt.get(), 14);
    out_cam.schedule_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 15));
    out_cam.ai_model = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 16));
    out_cam.enabled = sqlite3_column_int(stmt.get(), 17) != 0;
    out_cam.updated_at = sqlite3_column_int64(stmt.get(), 18);
    return true;
}

// ==========================================
// Video Recording Segment Timeline
// ==========================================

bool DatabaseManager::InsertRecordingSegment(const RecordingSegmentRecord& seg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR IGNORE INTO nvr_recordings (channel_id, start_time_ms, end_time_ms, duration_sec, "
                      "file_path, file_size_bytes, codec, trigger_type, is_locked, created_at_ms) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;

    sqlite3_bind_int(stmt.get(), 1, seg.channel_id);
    sqlite3_bind_int64(stmt.get(), 2, seg.start_time_ms);
    sqlite3_bind_int64(stmt.get(), 3, seg.end_time_ms);
    sqlite3_bind_int(stmt.get(), 4, seg.duration_sec);
    sqlite3_bind_text(stmt.get(), 5, seg.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 6, seg.file_size_bytes);
    sqlite3_bind_text(stmt.get(), 7, seg.codec.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, seg.trigger_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 9, seg.is_locked ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 10, seg.created_at_ms > 0 ? seg.created_at_ms : CurrentTimeMs());

    return stmt.step() == SQLITE_DONE;
}

std::vector<RecordingSegmentRecord> DatabaseManager::QueryRecordings(int channel_id, int64_t from_ms, int64_t to_ms,
                                                                    const std::string& trigger_type, int limit) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<RecordingSegmentRecord> results;
    if (!db_) return results;

    std::string sql = "SELECT id, channel_id, start_time_ms, end_time_ms, duration_sec, file_path, "
                      "file_size_bytes, codec, trigger_type, is_locked, created_at_ms "
                      "FROM nvr_recordings WHERE 1=1 ";

    if (channel_id > 0) sql += "AND channel_id = ? ";
    if (from_ms > 0) sql += "AND end_time_ms >= ? ";
    if (to_ms > 0) sql += "AND start_time_ms <= ? ";
    if (!trigger_type.empty()) sql += "AND trigger_type = ? ";

    sql += "ORDER BY start_time_ms DESC LIMIT ?;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return results;

    int idx = 1;
    if (channel_id > 0) sqlite3_bind_int(stmt.get(), idx++, channel_id);
    if (from_ms > 0) sqlite3_bind_int64(stmt.get(), idx++, from_ms);
    if (to_ms > 0) sqlite3_bind_int64(stmt.get(), idx++, to_ms);
    if (!trigger_type.empty()) sqlite3_bind_text(stmt.get(), idx++, trigger_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), idx++, limit > 0 ? limit : 200);

    while (stmt.step() == SQLITE_ROW) {
        RecordingSegmentRecord r;
        r.id = sqlite3_column_int64(stmt.get(), 0);
        r.channel_id = sqlite3_column_int(stmt.get(), 1);
        r.start_time_ms = sqlite3_column_int64(stmt.get(), 2);
        r.end_time_ms = sqlite3_column_int64(stmt.get(), 3);
        r.duration_sec = sqlite3_column_int(stmt.get(), 4);
        r.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        r.file_size_bytes = sqlite3_column_int64(stmt.get(), 6);
        r.codec = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        r.trigger_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        r.is_locked = sqlite3_column_int(stmt.get(), 9) != 0;
        r.created_at_ms = sqlite3_column_int64(stmt.get(), 10);
        results.push_back(r);
    }
    return results;
}

std::vector<std::string> DatabaseManager::GetRecordingDates(int channel_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> dates;
    if (!db_) return dates;

    std::string sql = "SELECT DISTINCT strftime('%Y-%m-%d', start_time_ms / 1000, 'unixepoch', 'localtime') as d "
                      "FROM nvr_recordings WHERE start_time_ms > 0 ";
    if (channel_id > 0) sql += "AND channel_id = ? ";
    sql += "ORDER BY d DESC;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return dates;
    if (channel_id > 0) sqlite3_bind_int(stmt.get(), 1, channel_id);

    while (stmt.step() == SQLITE_ROW) {
        const char* d = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (d && std::strlen(d) > 0) {
            dates.push_back(d);
        }
    }
    return dates;
}

bool DatabaseManager::GetRecordingStats(uint32_t& out_count, uint64_t& out_total_bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out_count = 0;
    out_total_bytes = 0;
    if (!db_) return false;

    Statement stmt(db_, "SELECT COUNT(*), COALESCE(SUM(file_size_bytes), 0) FROM nvr_recordings;");
    if (!stmt.isValid()) return false;

    if (stmt.step() == SQLITE_ROW) {
        out_count = static_cast<uint32_t>(sqlite3_column_int(stmt.get(), 0));
        out_total_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 1));
        return true;
    }
    return false;
}

bool DatabaseManager::SetRecordingLocked(int64_t segment_id, bool locked) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "UPDATE nvr_recordings SET is_locked = ? WHERE id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, locked ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 2, segment_id);

    return stmt.step() == SQLITE_DONE;
}

bool DatabaseManager::SetRecordingLockedByPath(const std::string& file_path, bool locked) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || file_path.empty()) return false;

    Statement stmt(db_, "UPDATE nvr_recordings SET is_locked = ? WHERE file_path = ? OR file_path LIKE ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int(stmt.get(), 1, locked ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
    std::string pattern = "%" + file_path;
    sqlite3_bind_text(stmt.get(), 3, pattern.c_str(), -1, SQLITE_TRANSIENT);

    return stmt.step() == SQLITE_DONE;
}

std::vector<RecordingSegmentRecord> DatabaseManager::GetOldestUnlockedSegments(int limit) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<RecordingSegmentRecord> results;
    if (!db_) return results;

    Statement stmt(db_, "SELECT id, channel_id, start_time_ms, end_time_ms, duration_sec, file_path, "
                        "file_size_bytes, codec, trigger_type, is_locked, created_at_ms "
                        "FROM nvr_recordings WHERE is_locked = 0 ORDER BY start_time_ms ASC LIMIT ?;");
    if (!stmt.isValid()) return results;
    sqlite3_bind_int(stmt.get(), 1, limit > 0 ? limit : 50);

    while (stmt.step() == SQLITE_ROW) {
        RecordingSegmentRecord r;
        r.id = sqlite3_column_int64(stmt.get(), 0);
        r.channel_id = sqlite3_column_int(stmt.get(), 1);
        r.start_time_ms = sqlite3_column_int64(stmt.get(), 2);
        r.end_time_ms = sqlite3_column_int64(stmt.get(), 3);
        r.duration_sec = sqlite3_column_int(stmt.get(), 4);
        r.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        r.file_size_bytes = sqlite3_column_int64(stmt.get(), 6);
        r.codec = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        r.trigger_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        r.is_locked = false;
        r.created_at_ms = sqlite3_column_int64(stmt.get(), 10);
        results.push_back(r);
    }
    return results;
}

bool DatabaseManager::DeleteRecordingSegment(int64_t segment_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "DELETE FROM nvr_recordings WHERE id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int64(stmt.get(), 1, segment_id);

    return stmt.step() == SQLITE_DONE;
}

bool DatabaseManager::DeleteRecordingSegmentByPath(const std::string& file_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || file_path.empty()) return false;

    Statement stmt(db_, "DELETE FROM nvr_recordings WHERE file_path = ? OR file_path LIKE ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_text(stmt.get(), 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    std::string like_pattern = "%" + file_path;
    sqlite3_bind_text(stmt.get(), 2, like_pattern.c_str(), -1, SQLITE_TRANSIENT);

    return stmt.step() == SQLITE_DONE;
}

// ==========================================
// Events & Alerts
// ==========================================

int64_t DatabaseManager::InsertEvent(const EventRecord& ev) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = "INSERT INTO nvr_events (channel_id, event_type, severity, start_time_ms, end_time_ms, "
                      "confidence, details_json, thumbnail_path, has_clip, is_acknowledged, created_at_ms) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return -1;

    sqlite3_bind_int(stmt.get(), 1, ev.channel_id);
    sqlite3_bind_text(stmt.get(), 2, ev.event_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, ev.severity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, ev.start_time_ms);
    sqlite3_bind_int64(stmt.get(), 5, ev.end_time_ms > 0 ? ev.end_time_ms : ev.start_time_ms);
    sqlite3_bind_double(stmt.get(), 6, static_cast<double>(ev.confidence));
    sqlite3_bind_text(stmt.get(), 7, ev.details_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, ev.thumbnail_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 9, ev.has_clip ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 10, ev.is_acknowledged ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 11, ev.created_at_ms > 0 ? ev.created_at_ms : CurrentTimeMs());

    if (stmt.step() != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

std::vector<EventRecord> DatabaseManager::QueryEvents(int channel_id, const std::string& event_type,
                                                      int64_t from_ms, int64_t to_ms, int limit) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EventRecord> events;
    if (!db_) return events;

    std::string sql = "SELECT id, channel_id, event_type, severity, start_time_ms, end_time_ms, "
                      "confidence, details_json, thumbnail_path, has_clip, is_acknowledged, created_at_ms "
                      "FROM nvr_events WHERE 1=1 ";

    if (channel_id > 0) sql += "AND channel_id = ? ";
    if (!event_type.empty() && event_type != "all") sql += "AND event_type = ? ";
    if (from_ms > 0) sql += "AND start_time_ms >= ? ";
    if (to_ms > 0) sql += "AND start_time_ms <= ? ";

    sql += "ORDER BY start_time_ms DESC LIMIT ?;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return events;

    int idx = 1;
    if (channel_id > 0) sqlite3_bind_int(stmt.get(), idx++, channel_id);
    if (!event_type.empty() && event_type != "all") sqlite3_bind_text(stmt.get(), idx++, event_type.c_str(), -1, SQLITE_TRANSIENT);
    if (from_ms > 0) sqlite3_bind_int64(stmt.get(), idx++, from_ms);
    if (to_ms > 0) sqlite3_bind_int64(stmt.get(), idx++, to_ms);
    sqlite3_bind_int(stmt.get(), idx++, limit > 0 ? limit : 100);

    while (stmt.step() == SQLITE_ROW) {
        EventRecord e;
        e.id = sqlite3_column_int64(stmt.get(), 0);
        e.channel_id = sqlite3_column_int(stmt.get(), 1);
        e.event_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        e.severity = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        e.start_time_ms = sqlite3_column_int64(stmt.get(), 4);
        e.end_time_ms = sqlite3_column_int64(stmt.get(), 5);
        e.confidence = static_cast<float>(sqlite3_column_double(stmt.get(), 6));
        e.details_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        e.thumbnail_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        e.has_clip = sqlite3_column_int(stmt.get(), 9) != 0;
        e.is_acknowledged = sqlite3_column_int(stmt.get(), 10) != 0;
        e.created_at_ms = sqlite3_column_int64(stmt.get(), 11);
        events.push_back(e);
    }
    return events;
}

bool DatabaseManager::AcknowledgeEvent(int64_t event_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    Statement stmt(db_, "UPDATE nvr_events SET is_acknowledged = 1 WHERE id = ?;");
    if (!stmt.isValid()) return false;
    sqlite3_bind_int64(stmt.get(), 1, event_id);

    return stmt.step() == SQLITE_DONE;
}

// ==========================================
// AI Configs
// ==========================================

bool DatabaseManager::UpsertAiConfig(const AiConfigRecord& cfg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"SQL(
        INSERT INTO nvr_ai_configs (channel_id, model_type, sensitivity, zones_json, schedule_json, enabled)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(channel_id, model_type) DO UPDATE SET
            sensitivity = excluded.sensitivity,
            zones_json = excluded.zones_json,
            schedule_json = excluded.schedule_json,
            enabled = excluded.enabled;
    )SQL";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;

    sqlite3_bind_int(stmt.get(), 1, cfg.channel_id);
    sqlite3_bind_text(stmt.get(), 2, cfg.model_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 3, cfg.sensitivity);
    sqlite3_bind_text(stmt.get(), 4, cfg.zones_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, cfg.schedule_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 6, cfg.enabled ? 1 : 0);

    return stmt.step() == SQLITE_DONE;
}

std::vector<AiConfigRecord> DatabaseManager::GetAiConfigs(int channel_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<AiConfigRecord> results;
    if (!db_) return results;

    std::string sql = "SELECT channel_id, model_type, sensitivity, zones_json, schedule_json, enabled "
                      "FROM nvr_ai_configs ";
    if (channel_id > 0) sql += "WHERE channel_id = ? ";
    sql += "ORDER BY channel_id ASC, model_type ASC;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return results;
    if (channel_id > 0) sqlite3_bind_int(stmt.get(), 1, channel_id);

    while (stmt.step() == SQLITE_ROW) {
        AiConfigRecord a;
        a.channel_id = sqlite3_column_int(stmt.get(), 0);
        a.model_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        a.sensitivity = sqlite3_column_int(stmt.get(), 2);
        a.zones_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        a.schedule_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        a.enabled = sqlite3_column_int(stmt.get(), 5) != 0;
        results.push_back(a);
    }
    return results;
}

// ==========================================
// System Settings
// ==========================================

bool DatabaseManager::SetSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || key.empty()) return false;

    int64_t now = CurrentTimeMs();
    const char* sql = "INSERT INTO nvr_system_settings (key, value, updated_at) VALUES (?, ?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;

    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, now);

    return stmt.step() == SQLITE_DONE;
}

std::string DatabaseManager::GetSetting(const std::string& key, const std::string& default_val) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || key.empty()) return default_val;

    Statement stmt(db_, "SELECT value FROM nvr_system_settings WHERE key = ?;");
    if (!stmt.isValid()) return default_val;
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);

    if (stmt.step() != SQLITE_ROW) return default_val;
    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return val ? std::string(val) : default_val;
}

std::map<std::string, std::string> DatabaseManager::GetAllSettings() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::map<std::string, std::string> map;
    if (!db_) return map;

    Statement stmt(db_, "SELECT key, value FROM nvr_system_settings;");
    if (!stmt.isValid()) return map;

    while (stmt.step() == SQLITE_ROW) {
        std::string k = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        std::string v = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        map[k] = v;
    }
    return map;
}

// ==========================================
// Audit Trail
// ==========================================

bool DatabaseManager::AppendAuditLog(const std::string& actor_username, const std::string& actor_role,
                                     const std::string& device_ip, const std::string& action,
                                     const std::string& section, const std::string& summary,
                                     const std::string& details_json, const std::string& status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    int64_t now = CurrentTimeMs();
    const char* sql = "INSERT INTO nvr_audit_log (timestamp_ms, actor_username, actor_role, device_ip, "
                      "action, section, summary, details_json, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return false;

    sqlite3_bind_int64(stmt.get(), 1, now);
    sqlite3_bind_text(stmt.get(), 2, actor_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, actor_role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, device_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, section.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, details_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, status.c_str(), -1, SQLITE_TRANSIENT);

    return stmt.step() == SQLITE_DONE;
}

std::vector<AuditLogRecord> DatabaseManager::QueryAuditLogs(int limit, int64_t before_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<AuditLogRecord> logs;
    if (!db_) return logs;

    std::string sql = "SELECT id, timestamp_ms, actor_username, actor_role, device_ip, action, section, summary, details_json, status "
                      "FROM nvr_audit_log ";
    if (before_ms > 0) sql += "WHERE timestamp_ms < ? ";
    sql += "ORDER BY timestamp_ms DESC LIMIT ?;";

    Statement stmt(db_, sql);
    if (!stmt.isValid()) return logs;

    int idx = 1;
    if (before_ms > 0) sqlite3_bind_int64(stmt.get(), idx++, before_ms);
    sqlite3_bind_int(stmt.get(), idx++, limit > 0 ? limit : 100);

    while (stmt.step() == SQLITE_ROW) {
        AuditLogRecord a;
        a.id = sqlite3_column_int64(stmt.get(), 0);
        a.timestamp_ms = sqlite3_column_int64(stmt.get(), 1);
        a.actor_username = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        a.actor_role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        a.device_ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        a.action = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        a.section = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        a.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        a.details_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        a.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
        logs.push_back(a);
    }
    return logs;
}

} // namespace nvr
