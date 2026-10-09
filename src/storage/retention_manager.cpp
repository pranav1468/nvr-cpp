#include "nvr/storage/retention_manager.h"
#include "nvr/storage/segment_index.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <sys/statvfs.h>
#include <filesystem>
#include <chrono>

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
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LOG_INFO << "RetentionManager stopped";
}

uint64_t RetentionManager::GetFreeDiskSpaceBytes(const std::string& path) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(stat.f_bavail) * static_cast<uint64_t>(stat.f_frsize);
}

uint64_t RetentionManager::GetTotalDiskSpaceBytes(const std::string& path) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(stat.f_blocks) * static_cast<uint64_t>(stat.f_frsize);
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

    int64_t now_ms = time_utils::WallTimeMs();
    int64_t max_retention_ms = static_cast<int64_t>(cfg.max_retention_days) * 86400LL * 1000LL;
    int64_t cutoff_ms = now_ms - max_retention_ms;

    // Check disk low space condition
    bool low_space = (free_bytes < min_free_bytes);

    if (low_space) {
        LOG_WARN << "Low storage detected! Free space: " << (free_bytes / (1024 * 1024))
                 << " MB < Threshold: " << cfg.min_free_space_mb << " MB. Initiating FIFO prune...";
    }

    // Retrieve batches of oldest unlocked segments
    while (low_space) {
        if (worker_thread_.joinable() && !running_) {
            break;
        }
        auto segments = SegmentIndex::Instance().GetOldestUnlockedSegments(10);
        if (segments.empty()) {
            LOG_WARN << "RetentionManager: No unlocked segments available to prune!";
            break;
        }

        bool made_progress = false;
        for (const auto& seg : segments) {
            bool file_removed = false;
            try {
                if (!std::filesystem::exists(seg.file_path)) {
                    file_removed = true;
                } else if (std::filesystem::remove(seg.file_path)) {
                    file_removed = true;
                    LOG_INFO << "Pruned old segment: " << seg.file_path << " (Freed "
                             << (seg.file_size_bytes / 1024) << " KB)";
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to delete file " << seg.file_path << ": " << e.what();
            }

            if (file_removed) {
                if (SegmentIndex::Instance().DeleteSegmentRecord(seg.id)) {
                    made_progress = true;
                }
            } else {
                LOG_WARN << "RetentionManager: Retaining DB record for locked/unremoved file: " << seg.file_path;
            }
        }

        if (!made_progress) {
            LOG_WARN << "RetentionManager: Pruning made no progress; stopping iteration to prevent infinite loop";
            break;
        }

        free_bytes = GetFreeDiskSpaceBytes(check_path);
        low_space = (free_bytes < min_free_bytes);
    }

    // Also prune segments exceeding max_retention_days if set
    if (cfg.max_retention_days > 0) {
        auto old_segments = SegmentIndex::Instance().GetOldestUnlockedSegments(50);
        for (const auto& seg : old_segments) {
            if (seg.start_time_ms < cutoff_ms) {
                bool file_removed = false;
                try {
                    if (!std::filesystem::exists(seg.file_path)) {
                        file_removed = true;
                    } else if (std::filesystem::remove(seg.file_path)) {
                        file_removed = true;
                        LOG_INFO << "Pruned expired segment (age > " << cfg.max_retention_days 
                                 << " days): " << seg.file_path;
                    }
                } catch (...) {}

                if (file_removed) {
                    SegmentIndex::Instance().DeleteSegmentRecord(seg.id);
                }
            } else {
                break; // Since results are sorted by start_time_ms ASC, remaining are newer
            }
        }
    }
}

void RetentionManager::RetentionLoop() {
    while (running_) {
        EnforceRetentionOnce();
        for (int i = 0; i < 60 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace nvr
