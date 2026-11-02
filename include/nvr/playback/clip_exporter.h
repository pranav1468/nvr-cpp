#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <cstdint>
#include "3rdparty/nlohmann/json.hpp"

namespace nvr {

enum class ExportState {
    PENDING = 0,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELLED
};

inline const char* ExportStateToString(ExportState s) {
    switch (s) {
        case ExportState::PENDING:   return "PENDING";
        case ExportState::RUNNING:   return "RUNNING";
        case ExportState::COMPLETED: return "COMPLETED";
        case ExportState::FAILED:    return "FAILED";
        case ExportState::CANCELLED: return "CANCELLED";
        default:                     return "UNKNOWN";
    }
}

struct ExportJob {
    std::string job_id;
    int channel_id{1};
    int64_t start_time_ms{0};
    int64_t end_time_ms{0};
    std::string output_filename;
    std::string output_path;
    ExportState state{ExportState::PENDING};
    int progress_percent{0};
    std::string error_message;
    uint64_t output_file_size{0};
    int64_t created_at_ms{0};
};

class ClipExporter {
public:
    static ClipExporter& Instance();

    void Configure(const std::string& export_directory);

    // Start an asynchronous clip export job
    std::string StartExport(int channel_id, int64_t start_time_ms, int64_t end_time_ms,
                            const std::string& custom_filename = "",
                            const std::vector<std::string>& segment_files = {});

    // Query job status
    nlohmann::json GetJobStatus(const std::string& job_id) const;

    // List all export jobs
    nlohmann::json ListJobs() const;

    // Cancel an ongoing job
    bool CancelJob(const std::string& job_id);

    // Stream-copy MP4 concatenation of multiple segment files into target_path (zero transcoding)
    static bool StitchMp4Segments(const std::vector<std::string>& input_files,
                                 const std::string& output_path,
                                 std::atomic<int>& progress_percent,
                                 std::atomic<bool>& cancel_flag,
                                 std::string& out_error);

private:
    ClipExporter();
    ~ClipExporter();
    ClipExporter(const ClipExporter&) = delete;
    ClipExporter& operator=(const ClipExporter&) = delete;

    void ExecuteJob(ExportJob job, std::vector<std::string> segments);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ExportJob> jobs_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> cancel_flags_;
    std::string export_dir_{"./recordings/exports"};
    std::atomic<bool> shutting_down_{false};
};

} // namespace nvr
