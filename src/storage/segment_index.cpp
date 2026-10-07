#include "nvr/storage/segment_index.h"
#include "nvr/storage/database_manager.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "3rdparty/sqlite3.h"

namespace nvr {

SegmentIndex& SegmentIndex::Instance() {
    static SegmentIndex instance;
    return instance;
}

bool SegmentIndex::InsertSegment(const SegmentMetadata& meta) {
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        LOG_ERROR << "SegmentIndex::InsertSegment: Database not open";
        return false;
    }

    const char* sql = 
        "INSERT INTO recordings ("
        "  channel_id, file_path, start_time_ms, end_time_ms, duration_ms, "
        "  file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps, created_at"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "Failed to prepare insert segment statement: " << sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_int(stmt, 1, meta.channel_id);
    sqlite3_bind_text(stmt, 2, meta.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, meta.start_time_ms);
    sqlite3_bind_int64(stmt, 4, meta.end_time_ms);
    sqlite3_bind_int64(stmt, 5, meta.duration_ms);
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(meta.file_size_bytes));
    sqlite3_bind_int(stmt, 7, static_cast<int>(meta.frame_count));
    sqlite3_bind_int(stmt, 8, static_cast<int>(meta.keyframe_count));
    sqlite3_bind_int(stmt, 9, meta.is_locked ? 1 : 0);
    sqlite3_bind_text(stmt, 10, CodecToString(meta.codec), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, static_cast<int>(meta.width));
    sqlite3_bind_int(stmt, 12, static_cast<int>(meta.height));
    sqlite3_bind_int(stmt, 13, static_cast<int>(meta.fps));
    sqlite3_bind_int64(stmt, 14, time_utils::WallTimeMs());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR << "Failed to insert segment for channel " << meta.channel_id << ": " << sqlite3_errmsg(db);
        return false;
    }

    LOG_DEBUG << "Indexed segment: " << meta.file_path << " (" << meta.duration_ms << "ms, " 
              << meta.file_size_bytes << " bytes)";
    return true;
}

std::vector<SegmentMetadata> SegmentIndex::QuerySegments(int channel_id, int64_t start_ms, int64_t end_ms) {
    std::vector<SegmentMetadata> results;
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return results;
    }

    const char* sql = 
        "SELECT id, channel_id, file_path, start_time_ms, end_time_ms, duration_ms, "
        "       file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps "
        "FROM recordings "
        "WHERE channel_id = ? AND end_time_ms >= ? AND start_time_ms <= ? "
        "ORDER BY start_time_ms ASC;";

    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int(stmt, 1, channel_id);
    sqlite3_bind_int64(stmt, 2, start_ms);
    sqlite3_bind_int64(stmt, 3, end_ms);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SegmentMetadata m;
        m.id = sqlite3_column_int64(stmt, 0);
        m.channel_id = sqlite3_column_int(stmt, 1);
        m.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.start_time_ms = sqlite3_column_int64(stmt, 3);
        m.end_time_ms = sqlite3_column_int64(stmt, 4);
        m.duration_ms = sqlite3_column_int64(stmt, 5);
        m.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        m.frame_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 7));
        m.keyframe_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 8));
        m.is_locked = (sqlite3_column_int(stmt, 9) != 0);
        const char* codec_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        m.codec = (codec_str && std::string(codec_str) == "H265") ? CodecType::H265 : CodecType::H264;
        m.width = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
        m.height = static_cast<uint32_t>(sqlite3_column_int(stmt, 12));
        m.fps = static_cast<uint32_t>(sqlite3_column_int(stmt, 13));
        results.push_back(m);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SegmentMetadata> SegmentIndex::GetOldestUnlockedSegments(int limit) {
    std::vector<SegmentMetadata> results;
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return results;
    }

    const char* sql = 
        "SELECT id, channel_id, file_path, start_time_ms, end_time_ms, duration_ms, "
        "       file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps "
        "FROM recordings "
        "WHERE is_locked = 0 "
        "ORDER BY start_time_ms ASC "
        "LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SegmentMetadata m;
        m.id = sqlite3_column_int64(stmt, 0);
        m.channel_id = sqlite3_column_int(stmt, 1);
        m.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.start_time_ms = sqlite3_column_int64(stmt, 3);
        m.end_time_ms = sqlite3_column_int64(stmt, 4);
        m.duration_ms = sqlite3_column_int64(stmt, 5);
        m.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        m.frame_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 7));
        m.keyframe_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 8));
        m.is_locked = false;
        const char* codec_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        m.codec = (codec_str && std::string(codec_str) == "H265") ? CodecType::H265 : CodecType::H264;
        m.width = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
        m.height = static_cast<uint32_t>(sqlite3_column_int(stmt, 12));
        m.fps = static_cast<uint32_t>(sqlite3_column_int(stmt, 13));
        results.push_back(m);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool SegmentIndex::DeleteSegmentRecord(int64_t id) {
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return false;
    }

    const char* sql = "DELETE FROM recordings WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

bool SegmentIndex::LockSegment(int64_t id, bool is_locked) {
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return false;
    }

    const char* sql = "UPDATE recordings SET is_locked = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, is_locked ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

uint64_t SegmentIndex::GetTotalRecordingSizeBytes() {
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return 0;
    }

    const char* sql = "SELECT COALESCE(SUM(file_size), 0) FROM recordings;";
    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    uint64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return total;
}

int SegmentIndex::GetTotalSegmentCount() {
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM recordings;";
    sqlite3_stmt* stmt = nullptr;
    std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

} // namespace nvr
