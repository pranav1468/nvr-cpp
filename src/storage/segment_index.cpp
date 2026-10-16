#include "nvr/storage/segment_index.h"
#include "nvr/storage/database_manager.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "3rdparty/sqlite3.h"

#include <filesystem>
#include <unordered_set>
#include <fstream>
#include <ctime>
#include <cstdio>


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
        "  file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps, "
        "  has_audio, audio_codec, created_at"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

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
    sqlite3_bind_int(stmt, 14, meta.has_audio ? 1 : 0);
    sqlite3_bind_text(stmt, 15, CodecToString(meta.audio_codec), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 16, time_utils::WallTimeMs());

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
        "       file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps, "
        "       has_audio, audio_codec "
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
        m.has_audio = (sqlite3_column_int(stmt, 14) != 0);
        const char* a_codec_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        m.audio_codec = a_codec_str ? StringToCodec(a_codec_str) : CodecType::UNKNOWN;
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
        "       file_size, frame_count, keyframe_count, is_locked, codec, width, height, fps, "
        "       has_audio, audio_codec "
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
        m.has_audio = (sqlite3_column_int(stmt, 14) != 0);
        const char* a_codec_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        m.audio_codec = a_codec_str ? StringToCodec(a_codec_str) : CodecType::UNKNOWN;
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

ReconciliationReport SegmentIndex::ReconcileStorage(const std::string& recording_path) {
    ReconciliationReport report;
    sqlite3* db = DatabaseManager::Instance().GetHandle();
    if (!db) {
        LOG_ERROR << "SegmentIndex::ReconcileStorage: Database not open";
        return report;
    }

    // Safety guardrail: ensure recording_path exists and is accessible before touching DB
    std::error_code ec;
    if (!std::filesystem::exists(recording_path, ec) || !std::filesystem::is_directory(recording_path, ec)) {
        LOG_WARN << "Reconciliation skipped: storage path is not accessible or not a directory: " << recording_path;
        return report;
    }

    // Phase 1: DB -> Disk (Purge ghost records whose physical files are missing)
    std::unordered_set<std::string> known_paths;
    std::vector<int64_t> ghost_ids;

    {
        std::lock_guard<std::mutex> lock(DatabaseManager::Instance().GetMutex());
        const char* select_sql = "SELECT id, file_path FROM recordings;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* path_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (path_txt) {
                    std::string path_str(path_txt);
                    if (!std::filesystem::exists(path_str, ec)) {
                        ghost_ids.push_back(id);
                    } else {
                        known_paths.insert(path_str);
                    }
                }
            }
            sqlite3_finalize(stmt);
        }

        // Purge ghost IDs
        if (!ghost_ids.empty()) {
            sqlite3_stmt* del_stmt = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM recordings WHERE id = ?;", -1, &del_stmt, nullptr) == SQLITE_OK) {
                for (int64_t gid : ghost_ids) {
                    sqlite3_bind_int64(del_stmt, 1, gid);
                    if (sqlite3_step(del_stmt) == SQLITE_DONE) {
                        report.ghost_records_pruned++;
                    }
                    sqlite3_reset(del_stmt);
                }
                sqlite3_finalize(del_stmt);
            }
        }
    }

    if (report.ghost_records_pruned > 0) {
        LOG_INFO << "Reconciliation: Pruned " << report.ghost_records_pruned
                 << " ghost database records (file missing on storage)";
    }

    // Phase 2: Disk -> DB (Scan for unindexed .mp4 files on disk)
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 recording_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;

            const auto& p = entry.path();
            if (p.extension() != ".mp4") continue;

            std::string file_path_str = p.string();
            if (known_paths.find(file_path_str) != known_paths.end()) {
                continue; // Already indexed
            }

            // Unindexed .mp4 discovered
            uintmax_t file_size = entry.file_size(ec);
            if (ec || file_size == 0) {
                std::filesystem::remove(p, ec);
                report.corrupt_files_removed++;
                continue;
            }

            // Validate MP4 header (must be at least 16 bytes and contain 'ftyp' box)
            bool is_valid_mp4 = false;
            std::ifstream ifs(file_path_str, std::ios::binary);
            if (ifs) {
                char header[16] = {0};
                ifs.read(header, 16);
                if (ifs.gcount() >= 8) {
                    if (header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p') {
                        is_valid_mp4 = true;
                    }
                }
                ifs.close();
            }

            if (!is_valid_mp4) {
                LOG_WARN << "Reconciliation: Removing corrupt unindexed file (missing ftyp box): " << file_path_str;
                std::filesystem::remove(p, ec);
                report.corrupt_files_removed++;
                continue;
            }

            // Parse metadata from filename: "cam<channel_id>_<YYYYMMDD_HHMMSS>_<mmm>.mp4"
            std::string filename = p.filename().string();
            int channel_id = 0;
            int64_t start_time_ms = 0;

            int ch = 0, year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
            if (std::sscanf(filename.c_str(), "cam%d_%4d%2d%2d_%2d%2d%2d_%d",
                            &ch, &year, &mon, &day, &hour, &min, &sec, &ms) == 8) {
                channel_id = ch;
                std::tm tm{};
                tm.tm_year = year - 1900;
                tm.tm_mon = mon - 1;
                tm.tm_mday = day;
                tm.tm_hour = hour;
                tm.tm_min = min;
                tm.tm_sec = sec;
                tm.tm_isdst = -1;
                std::time_t t = std::mktime(&tm);
                if (t != static_cast<std::time_t>(-1)) {
                    start_time_ms = static_cast<int64_t>(t) * 1000 + ms;
                }
            } else {
                auto cam_pos = filename.find("cam");
                auto under_pos = filename.find('_');
                if (cam_pos == 0 && under_pos != std::string::npos && under_pos > 3) {
                    try {
                        channel_id = std::stoi(filename.substr(3, under_pos - 3));
                    } catch (...) {
                        channel_id = 0;
                    }
                }
            }

            if (start_time_ms <= 0) {
                start_time_ms = time_utils::WallTimeMs();
            }

            int64_t duration_ms = 60000;
            int64_t end_time_ms = start_time_ms + duration_ms;

            SegmentMetadata meta;
            meta.channel_id = channel_id;
            meta.file_path = file_path_str;
            meta.start_time_ms = start_time_ms;
            meta.end_time_ms = end_time_ms;
            meta.duration_ms = duration_ms;
            meta.file_size_bytes = file_size;
            meta.frame_count = static_cast<uint32_t>((duration_ms * 25) / 1000);
            meta.keyframe_count = static_cast<uint32_t>(meta.frame_count / 25);
            meta.is_locked = false;
            meta.codec = CodecType::H264;
            meta.width = 1920;
            meta.height = 1080;
            meta.fps = 25;
            meta.has_audio = false;
            meta.audio_codec = CodecType::UNKNOWN;

            if (InsertSegment(meta)) {
                known_paths.insert(file_path_str);
                report.unindexed_files_recovered++;
                LOG_INFO << "Reconciliation: Recovered and indexed orphan segment: " << file_path_str;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Reconciliation error during directory walk: " << e.what();
    }

    LOG_INFO << "Reconciliation finished: " << report.ghost_records_pruned << " ghost records pruned, "
             << report.unindexed_files_recovered << " orphan segments recovered, "
             << report.corrupt_files_removed << " corrupt files removed.";

    return report;
}

} // namespace nvr
