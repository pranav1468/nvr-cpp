#pragma once

#include "nvr/common/types.h"
#include "nvr/playback/seek_controller.h"
#include "nvr/playback/clip_exporter.h"
#include "3rdparty/nlohmann/json.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <filesystem>

namespace nvr {

struct RecordingQueryFilter {
    int channel_id{0};          // 0 = all channels
    std::string date;           // YYYYMMDD
    std::string trigger_type;   // "continuous", "motion", "ai", etc.
    std::string from_time;      // HH:MM
    std::string to_time;        // HH:MM
    int limit{200};
};

class PlaybackEngine {
public:
    static PlaybackEngine& Instance();

    void Initialize(const std::string& recording_root_path);

    // List available recording dates (sorted descending, e.g. ["20261102", "20261101"])
    std::vector<std::string> GetRecordingDates();

    // Query recordings matching filter, returning raw segments and review activity blocks
    nlohmann::json QueryRecordings(const RecordingQueryFilter& filter);
    nlohmann::json QueryRecordings(const nlohmann::json& filter_json);

    // Lock/unlock a segment or file against retention pruning
    bool SetSegmentLock(const std::string& file_path, bool locked);

    // Resolve a relative recording path to an absolute, validated local path
    std::string GetSegmentLocalPath(const std::string& relative_path);

    // Start a clip export job
    std::string StartExport(int channel_id, int64_t start_time_ms, int64_t end_time_ms,
                            const std::string& custom_filename = "");

    // Get status of an export job
    nlohmann::json GetExportStatus(const std::string& job_id);

    // List all export jobs
    nlohmann::json ListExportJobs();

    // Cancel an ongoing export job
    bool CancelExport(const std::string& job_id);

    // Create / get seek controller session
    std::shared_ptr<SeekController> GetOrCreateSeekController(int channel_id);

private:
    PlaybackEngine();
    ~PlaybackEngine() = default;
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    std::filesystem::path GetStorageRoot() const;

    mutable std::mutex mutex_;
    std::string recording_root_{"./recordings"};
    std::unordered_map<int, std::shared_ptr<SeekController>> seek_controllers_;
};

} // namespace nvr
