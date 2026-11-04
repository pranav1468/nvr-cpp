#include "nvr/storage/retention_manager.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/database/database_manager.h"

#include <sys/statvfs.h>
#include <filesystem>
#include <chrono>
#include <vector>
#include <algorithm>

namespace nvr {

RetentionManager& RetentionManager::Instance() {
    static RetentionManager instance;
    return instance;
}

RetentionManager::~RetentionManager() {
    Stop();
}

void RetentionManager::Configure(const StorageConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

void RetentionManager::Start() {
    if (running_.exchange(true)) {
        return;
    }
    LOG_INFO << "Starting RetentionManager worker (check interval: 60s, min free: " 
             << config_.min_free_space_mb << " MB)";
    worker_thread_ = std::thread(&RetentionManager::RetentionLoop, this);
}

void RetentionManager::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        cv_.notify_all();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LOG_INFO << "RetentionManager stopped";
}

namespace {
std::string ResolveExistingStatPath(const std::string& path) {
    std::error_code ec;
    std::filesystem::path p = path.empty() ? std::filesystem::current_path(ec) : std::filesystem::path(path);
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return p.empty() ? "." : p.string();
}

struct DiskSegment {
    std::filesystem::path path;
    uint64_t size_bytes{0};
    std::filesystem::file_time_type last_write_time;
};

void DeleteCompanionThumbnails(const std::filesystem::path& video_path) {
    std::error_code ec;
    std::string stem = video_path.stem().string();
    auto parent = video_path.parent_path();
    for (const char* ext : {".bmp", ".jpg", ".png", ".locked"}) {
        auto thumb = parent / (stem + ext);
        if (std::filesystem::exists(thumb, ec)) {
            std::filesystem::remove(thumb, ec);
        }
    }
}
} // namespace

uint64_t RetentionManager::GetFreeDiskSpaceBytes(const std::string& path) {
    struct statvfs stat;
    std::string resolved = ResolveExistingStatPath(path);
    if (statvfs(resolved.c_str(), &stat) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(stat.f_bavail) * static_cast<uint64_t>(stat.f_frsize);
}

uint64_t RetentionManager::GetTotalDiskSpaceBytes(const std::string& path) {
    struct statvfs stat;
    std::string resolved = ResolveExistingStatPath(path);
    if (statvfs(resolved.c_str(), &stat) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(stat.f_blocks) * static_cast<uint64_t>(stat.f_frsize);
}

std::string RetentionManager::GetRecordingPath() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_.recording_path.empty() ? "./recordings" : config_.recording_path;
}

void RetentionManager::SetRecordingPath(const std::string& new_path) {
    if (new_path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_.recording_path = new_path;
    }
    std::error_code ec;
    std::filesystem::create_directories(new_path, ec);
    LOG_INFO << "RetentionManager: Recording storage path updated to " << new_path;
}

void RetentionManager::EnforceRetentionOnce() {
    StorageConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }

    std::string check_path = cfg.recording_path;
    try {
        if (!std::filesystem::exists(check_path)) {
            std::filesystem::create_directories(check_path);
        }
    } catch (...) {}

    uint64_t free_bytes = GetFreeDiskSpaceBytes(check_path);
    uint64_t min_free_bytes = static_cast<uint64_t>(cfg.min_free_space_mb) * 1024ULL * 1024ULL;

    // Discover all recorded .mp4 segments in the storage directory
    std::vector<DiskSegment> segments;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(check_path, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && it->path().extension() == ".mp4") {
            DiskSegment seg;
            seg.path = it->path();
            seg.size_bytes = it->file_size(ec);
            seg.last_write_time = it->last_write_time(ec);
            segments.push_back(seg);
        }
    }

    // Sort by modification time ascending (oldest first)
    std::sort(segments.begin(), segments.end(), [](const DiskSegment& a, const DiskSegment& b) {
        return a.last_write_time < b.last_write_time;
    });

    // Check disk low space condition and prune oldest files FIFO
    bool low_space = (free_bytes < min_free_bytes);
    if (low_space) {
        LOG_WARN << "Low storage detected! Free space: " << (free_bytes / (1024 * 1024))
                 << " MB < Threshold: " << cfg.min_free_space_mb << " MB. Initiating filesystem FIFO prune...";

        for (const auto& seg : segments) {
            if (worker_thread_.joinable() && !running_) break;
            if (!std::filesystem::exists(seg.path, ec)) continue;

            auto lock_file = seg.path.parent_path() / (seg.path.stem().string() + ".locked");
            if (std::filesystem::exists(lock_file, ec)) {
                continue; // Never delete locked footage
            }

            if (std::filesystem::remove(seg.path, ec)) {
                DeleteCompanionThumbnails(seg.path);
                DatabaseManager::Instance().DeleteRecordingSegmentByPath(seg.path.string());
                LOG_INFO << "Pruned old segment: " << seg.path.string() << " (Freed "
                         << (seg.size_bytes / 1024) << " KB)";
                free_bytes = GetFreeDiskSpaceBytes(check_path);
                if (free_bytes >= min_free_bytes) {
                    low_space = false;
                    break;
                }
            }
        }
    }

    // Age-based retention pruning if max_retention_days > 0
    if (cfg.max_retention_days > 0) {
        auto now_fs = std::filesystem::file_time_type::clock::now();
        int64_t max_age_sec = static_cast<int64_t>(cfg.max_retention_days) * 86400LL;
        for (const auto& seg : segments) {
            if (worker_thread_.joinable() && !running_) break;
            if (!std::filesystem::exists(seg.path, ec)) continue;

            auto lock_file = seg.path.parent_path() / (seg.path.stem().string() + ".locked");
            if (std::filesystem::exists(lock_file, ec)) {
                continue; // Never delete locked footage
            }

            auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now_fs - seg.last_write_time).count();
            if (age_sec > max_age_sec) {
                if (std::filesystem::remove(seg.path, ec)) {
                    DeleteCompanionThumbnails(seg.path);
                    DatabaseManager::Instance().DeleteRecordingSegmentByPath(seg.path.string());
                    LOG_INFO << "Pruned expired segment (age > " << cfg.max_retention_days 
                             << " days): " << seg.path.string();
                }
            }
        }
    }
}

void RetentionManager::RetentionLoop() {
    while (running_) {
        EnforceRetentionOnce();
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::seconds(60), [this] {
            return !running_.load();
        });
    }
}

} // namespace nvr
