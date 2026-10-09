#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/storage/database_manager.h"
#include "nvr/storage/segment_index.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/media/stream_broker.h"
#include "nvr/recording/atomic_writer.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/live/live_controller.h"
#include "3rdparty/nlohmann/json.hpp"

#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <filesystem>

using json = nlohmann::json;

namespace {
std::atomic<bool> g_running{true};

void SignalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        LOG_INFO << "Caught shutdown signal (" << signum << "), initiating clean termination...";
        g_running = false;
    }
}

nvr::NvrConfig LoadConfig(const std::string& config_path) {
    nvr::NvrConfig config;
    std::ifstream f(config_path);
    if (!f.is_open()) {
        LOG_WARN << "Config file not found at " << config_path << ", using defaults";
        return config;
    }

    try {
        json j;
        f >> j;

        if (j.contains("system")) {
            auto& sys = j["system"];
            if (sys.contains("device_name")) config.system.device_name = sys["device_name"];
            if (sys.contains("log_level")) config.system.log_level = sys["log_level"];
            if (sys.contains("max_channels")) config.system.max_channels = sys["max_channels"];
        }

        if (j.contains("storage")) {
            auto& st = j["storage"];
            if (st.contains("recording_path")) config.storage.recording_path = st["recording_path"];
            if (st.contains("database_path")) config.storage.database_path = st["database_path"];
            if (st.contains("segment_duration_seconds")) config.storage.segment_duration_seconds = st["segment_duration_seconds"];
            if (st.contains("min_free_space_mb")) config.storage.min_free_space_mb = st["min_free_space_mb"];
            if (st.contains("max_retention_days")) config.storage.max_retention_days = st["max_retention_days"];
        }

        if (j.contains("live")) {
            auto& lv = j["live"];
            if (lv.contains("grid_layout")) config.live.grid_layout = lv["grid_layout"];
            if (lv.contains("max_queue_depth")) config.live.max_queue_depth = lv["max_queue_depth"];
            if (lv.contains("drop_stale_frames")) config.live.drop_stale_frames = lv["drop_stale_frames"];
        }

        if (j.contains("cameras") && j["cameras"].is_array()) {
            for (const auto& item : j["cameras"]) {
                nvr::CameraConfig cam;
                cam.id = item.value("id", 1);
                cam.name = item.value("name", "Camera");
                cam.rtsp_url = item.value("rtsp_url", "");
                cam.sub_rtsp_url = item.value("sub_rtsp_url", "");
                cam.enabled = item.value("enabled", true);
                cam.record_mode = static_cast<nvr::RecordMode>(item.value("record_mode", 1));
                config.cameras.push_back(cam);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Error parsing config JSON: " << e.what();
    }

    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path = "config/nvr_config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    // Register signals
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Load configuration
    nvr::NvrConfig config = LoadConfig(config_path);

    // Configure logger
    if (config.system.log_level == "debug") {
        nvr::Logger::Instance().SetLogLevel(nvr::LogLevel::DEBUG);
    } else if (config.system.log_level == "warn") {
        nvr::Logger::Instance().SetLogLevel(nvr::LogLevel::WARN);
    } else if (config.system.log_level == "error") {
        nvr::Logger::Instance().SetLogLevel(nvr::LogLevel::ERROR);
    } else {
        nvr::Logger::Instance().SetLogLevel(nvr::LogLevel::INFO);
    }

    LOG_INFO << "==========================================================";
    LOG_INFO << "Starting NVR Production Core: " << config.system.device_name;
    LOG_INFO << "Target Storage: " << config.storage.recording_path;
    LOG_INFO << "Segment Duration: " << config.storage.segment_duration_seconds << "s";
    LOG_INFO << "Database File: " << config.storage.database_path;
    LOG_INFO << "==========================================================";

    // Startup reconciliation: clean orphaned .tmp segments
    nvr::AtomicWriter::CleanOrphanedTmpFiles(config.storage.recording_path);

    // Initialize Database
    if (!nvr::DatabaseManager::Instance().Initialize(config.storage.database_path)) {
        LOG_ERROR << "Fatal: Failed to initialize SQLite database. Exiting.";
        return 1;
    }

    // Initialize Retention Manager
    nvr::RetentionManager::Instance().Configure(config.storage);
    nvr::RetentionManager::Instance().Start();

    // Initialize Recording Scheduler
    nvr::RecordingScheduler::Instance().Configure(config.storage);

    // Initialize Camera Manager
    nvr::CameraManager::Instance().Initialize(config.cameras);

    // Start Recording and Ingress for configured cameras
    for (const auto& cam : config.cameras) {
        if (cam.enabled) {
            nvr::RecordingScheduler::Instance().StartChannelRecording(cam.id, cam.record_mode);
            nvr::CameraManager::Instance().StartCamera(cam.id);
        }
    }

    // Initialize Live View Pipeline
    nvr::LiveController::Instance().Configure(config.live);
    if (config.live.grid_layout == 1) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::SINGLE);
    } else if (config.live.grid_layout == 6) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_6);
    } else if (config.live.grid_layout == 8) {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_8);
    } else {
        nvr::LiveController::Instance().SetLayout(nvr::LiveGridLayout::GRID_4);
    }
    nvr::LiveController::Instance().Start();

    LOG_INFO << "NVR Recording and Live View Pipelines initialized and operational.";

    // Supervisor Telemetry Loop
    int loop_counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        loop_counter++;

        if (loop_counter % 10 == 0) {
            uint64_t free_mb = nvr::RetentionManager::Instance().GetFreeDiskSpaceBytes(config.storage.recording_path) / (1024 * 1024);
            int total_segs = nvr::SegmentIndex::Instance().GetTotalSegmentCount();
            uint64_t total_mb = nvr::SegmentIndex::Instance().GetTotalRecordingSizeBytes() / (1024 * 1024);

            LOG_INFO << "[System Telemetry] Storage Free: " << free_mb << " MB | Indexed Segments: " 
                     << total_segs << " (" << total_mb << " MB stored)";

            for (const auto& cam : config.cameras) {
                if (cam.enabled) {
                    bool online = nvr::CameraManager::Instance().IsCameraOnline(cam.id);
                    bool rec = nvr::RecordingScheduler::Instance().IsChannelRecording(cam.id);
                    uint32_t frames = nvr::RecordingScheduler::Instance().GetChannelFrameCount(cam.id);
                    uint64_t bytes = nvr::RecordingScheduler::Instance().GetChannelBytesWritten(cam.id);

                    LOG_INFO << "  -> [Cam " << cam.id << ": " << cam.name << "] Online: " 
                             << (online ? "YES" : "NO") << " | REC: " << (rec ? "ACTIVE" : "STANDBY")
                             << " | Frames: " << frames << " | Bytes: " << (bytes / 1024) << " KB";
                }
            }
        }
    }

    LOG_INFO << "Shutting down NVR subsystems gracefully...";

    // 1. Stop Live View Pipeline
    nvr::LiveController::Instance().Stop();

    // 2. Stop Camera Ingress
    nvr::CameraManager::Instance().StopAll();

    // 3. Flush and Finalize Recording Segments
    nvr::RecordingScheduler::Instance().StopAll();

    // 3. Stop Retention Manager
    nvr::RetentionManager::Instance().Stop();

    // 4. Close Database
    nvr::DatabaseManager::Instance().Close();

    LOG_INFO << "NVR Core cleanly terminated. All segments finalized.";
    return 0;
}
