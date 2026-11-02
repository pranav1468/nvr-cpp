#pragma once

#include "nvr/common/types.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace nvr {

class RetentionManager {
public:
    static RetentionManager& Instance();

    void Configure(const StorageConfig& config);
    void Start();
    void Stop();

    void EnforceRetentionOnce();

    uint64_t GetFreeDiskSpaceBytes(const std::string& path);
    uint64_t GetTotalDiskSpaceBytes(const std::string& path);
    std::string GetRecordingPath();

private:
    RetentionManager() = default;
    ~RetentionManager();
    RetentionManager(const RetentionManager&) = delete;
    RetentionManager& operator=(const RetentionManager&) = delete;

    void RetentionLoop();

    StorageConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::mutex config_mutex_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};

} // namespace nvr
