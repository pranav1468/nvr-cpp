#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>

struct sqlite3;

namespace nvr {

struct UserRecord {
    int id = 0;
    std::string username;
    std::string password_hash;
    std::string salt;
    std::string role = "VIEWER"; // 'ADMIN', 'OPERATOR', 'VIEWER'
    bool is_active = true;
    int failed_attempts = 0;
    int64_t locked_until = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

struct SessionRecord {
    std::string token;
    int user_id = 0;
    std::string username;
    std::string role;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

struct CameraRecord {
    int channel_id = 1; // 1..8
    std::string name;
    std::string ip_address;
    std::string onvif_xaddr;
    std::string onvif_username;
    std::string onvif_password; // Plain in memory, AES-GCM encrypted in SQLite
    std::string rtsp_main_url;
    std::string rtsp_sub_url;
    std::string codec = "H264";
    std::string resolution = "1080p";
    int fps = 25;
    int bitrate_kbps = 4096;
    int record_mode = 1; // 0: Off, 1: Continuous, 2: Event
    int pre_roll_sec = 5;
    int post_roll_sec = 10;
    std::string schedule_json = "{}";
    std::string ai_model = "None";
    bool enabled = true;
    int64_t updated_at = 0;
};

struct RecordingSegmentRecord {
    int64_t id = 0;
    int channel_id = 1;
    int64_t start_time_ms = 0;
    int64_t end_time_ms = 0;
    int duration_sec = 60;
    std::string file_path;
    int64_t file_size_bytes = 0;
    std::string codec = "H264";
    std::string trigger_type = "continuous"; // 'continuous', 'motion', 'event'
    bool is_locked = false;
    int64_t created_at_ms = 0;
};

struct EventRecord {
    int64_t id = 0;
    int channel_id = 1;
    std::string event_type = "motion";
    std::string severity = "info";
    int64_t start_time_ms = 0;
    int64_t end_time_ms = 0;
    float confidence = 0.0f;
    std::string details_json = "{}";
    std::string thumbnail_path;
    bool has_clip = false;
    bool is_acknowledged = false;
    int64_t created_at_ms = 0;
};

struct AiConfigRecord {
    int channel_id = 1;
    std::string model_type;
    int sensitivity = 75;
    std::string zones_json = "[]";
    std::string schedule_json = "{}";
    bool enabled = false;
};

struct AuditLogRecord {
    int64_t id = 0;
    int64_t timestamp_ms = 0;
    std::string actor_username;
    std::string actor_role;
    std::string device_ip;
    std::string action;
    std::string section;
    std::string summary;
    std::string details_json = "{}";
    std::string status = "SUCCESS";
};

class DatabaseManager final {
public:
    static DatabaseManager& Instance();

    bool Initialize(const std::string& db_path = "config/nvr.db");
    void Close();
    bool IsOpen() const;

    // --- User & RBAC Management ---
    bool CreateUser(const std::string& username, const std::string& password, const std::string& role, std::string* error = nullptr);
    bool VerifyUserCredentials(const std::string& username, const std::string& password, UserRecord& out_user, std::string* error = nullptr);
    bool ChangeUserPassword(int user_id, const std::string& old_password, const std::string& new_password, std::string* error = nullptr);
    bool AdminResetPassword(int user_id, const std::string& new_password, std::string* error = nullptr);
    bool DeleteUser(int user_id, std::string* error = nullptr);
    std::vector<UserRecord> ListUsers();
    bool GetUserById(int user_id, UserRecord& out_user);
    bool GetUserByUsername(const std::string& username, UserRecord& out_user);

    // --- Session Tokens ---
    std::string CreateSession(int user_id, int duration_sec = 86400 * 7);
    bool ValidateSession(const std::string& token, SessionRecord& out_session);
    bool RevokeSession(const std::string& token);
    void CleanupExpiredSessions();

    // --- Camera Inventory ---
    bool UpsertCamera(const CameraRecord& cam);
    bool DeleteCamera(int channel_id);
    std::vector<CameraRecord> GetCameras();
    bool GetCamera(int channel_id, CameraRecord& out_cam);

    // --- Video Recording Segment Timeline ---
    bool InsertRecordingSegment(const RecordingSegmentRecord& seg);
    std::vector<RecordingSegmentRecord> QueryRecordings(int channel_id, int64_t from_ms, int64_t to_ms,
                                                       const std::string& trigger_type = "", int limit = 200);
    std::vector<std::string> GetRecordingDates(int channel_id = 0);
    bool GetRecordingStats(uint32_t& out_count, uint64_t& out_total_bytes);
    bool SetRecordingLocked(int64_t segment_id, bool locked);
    bool SetRecordingLockedByPath(const std::string& file_path, bool locked);
    std::vector<RecordingSegmentRecord> GetOldestUnlockedSegments(int limit = 50);
    bool DeleteRecordingSegment(int64_t segment_id);
    bool DeleteRecordingSegmentByPath(const std::string& file_path);

    // --- Events & Alerts ---
    int64_t InsertEvent(const EventRecord& event);
    std::vector<EventRecord> QueryEvents(int channel_id, const std::string& event_type,
                                        int64_t from_ms, int64_t to_ms, int limit = 100);
    bool AcknowledgeEvent(int64_t event_id);

    // --- AI Configs ---
    bool UpsertAiConfig(const AiConfigRecord& cfg);
    std::vector<AiConfigRecord> GetAiConfigs(int channel_id = 0);

    // --- System Settings ---
    bool SetSetting(const std::string& key, const std::string& value);
    std::string GetSetting(const std::string& key, const std::string& default_val = "");
    std::map<std::string, std::string> GetAllSettings();

    // --- Audit Trail ---
    bool AppendAuditLog(const std::string& actor_username, const std::string& actor_role,
                        const std::string& device_ip, const std::string& action,
                        const std::string& section, const std::string& summary,
                        const std::string& details_json = "{}", const std::string& status = "SUCCESS");
    std::vector<AuditLogRecord> QueryAuditLogs(int limit = 100, int64_t before_ms = 0);

private:
    DatabaseManager() = default;
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool ExecuteSchema();
    void BootstrapDefaults();

    mutable std::recursive_mutex mutex_;
    sqlite3* db_ = nullptr;
    std::string db_path_;
    std::string vault_key_;
};

} // namespace nvr
