#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include "nvr/common/deep_tracer.h"
#include "nvr/common/time_utils.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/media/stream_broker.h"
#include "nvr/recording/atomic_writer.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/live/live_controller.h"
#include "nvr/live/qml_video_bridge.h"
#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/api/rest_server.h"
#include "nvr/common/config_loader.h"
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

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path = "config/nvr_config.json";
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-h") {
            std::cout << "Usage: " << argv[0] << " [config_file_path]\n"
                      << "  Default config: config/nvr_config.json\n";
            return 0;
        }
        if (arg1 == "--version" || arg1 == "-v") {
            std::cout << "NVR Production Core v2.0 (NXP i.MX 95 Target Architecture)\n";
            return 0;
        }
        config_path = arg1;
    }

    // Register signals
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Load configuration
    nvr::NvrConfig config = nvr::LoadNvrConfig(config_path);

#ifdef NVR_ENABLE_DEEP_MONITORING
    std::string log_file = config.system.log_dir + "/nvr_deep_trace.log";
    nvr::DeepTracer::Instance().Start(log_file, config.storage.recording_path);
#endif

    // Configure logger
    nvr::LogLevel log_lvl = nvr::LogLevel::INFO;
    if (config.system.log_level == "debug") {
        log_lvl = nvr::LogLevel::DEBUG;
    } else if (config.system.log_level == "warn") {
        log_lvl = nvr::LogLevel::WARN;
    } else if (config.system.log_level == "error") {
        log_lvl = nvr::LogLevel::ERROR;
    }
    nvr::Logger::Instance().SetConsoleLogLevel(log_lvl);
#ifndef NVR_ENABLE_DEEP_MONITORING
    nvr::Logger::Instance().SetLogLevel(log_lvl);
#endif

#ifdef NVR_ENABLE_DEEP_MONITORING
    nvr::DeepTracer::Instance().SetStoragePath(config.storage.recording_path);
#endif

    nvr::CommandDispatcher::Instance().SetDeviceName(config.system.device_name);

    LOG_INFO << "==========================================================";
    LOG_INFO << "Starting NVR Production Core: " << config.system.device_name;
    LOG_INFO << "Target Storage: " << config.storage.recording_path;
    LOG_INFO << "Segment Duration: " << config.storage.segment_duration_seconds << "s";
    LOG_INFO << "==========================================================";

    // Ensure recording storage directory exists before reconciliation and telemetry
    {
        std::error_code ec;
        std::filesystem::create_directories(config.storage.recording_path, ec);
    }

    // Startup reconciliation: clean orphaned .tmp segments
    nvr::AtomicWriter::CleanOrphanedTmpFiles(config.storage.recording_path);

    // Initialize Retention Manager
    nvr::RetentionManager::Instance().Configure(config.storage);
    nvr::RetentionManager::Instance().Start();

    // Initialize Recording Scheduler
    nvr::RecordingScheduler::Instance().Configure(config.storage);

    // Initialize Camera Manager
    nvr::CameraManager::Instance().SetMaxChannels(config.system.max_channels);
    nvr::CameraManager::Instance().SetConfigFilePath(config_path);
    nvr::CameraManager::Instance().Initialize(config.cameras);

    // Start Recording and Ingress for configured cameras
    for (const auto& cam : nvr::CameraManager::Instance().GetCameras()) {
        if (cam.enabled) {
            nvr::RecordingScheduler::Instance().StartChannelRecording(cam.id, cam.record_mode);
            nvr::CameraManager::Instance().StartCamera(cam.id);
            nvr::CameraManager::Instance().StartSubStream(cam.id);
        }
    }

    // Initialize Live View Pipeline with QmlVideoBridge display backend
    nvr::LiveController::Instance().SetDisplayBackend(
        std::shared_ptr<nvr::IDisplayBackend>(&nvr::QmlVideoBridge::Instance(), [](nvr::IDisplayBackend*){})
    );
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

    // Start HTTP REST Control Plane
    if (config.api.enabled) {
        nvr::RestServer::Instance().Configure(config.api);
        nvr::RestServer::Instance().Start();
    }

    LOG_INFO << "NVR Recording, Live View, and Control API initialized and operational.";

    // Supervisor Telemetry Loop
    int loop_counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        loop_counter++;

        // Periodic maintenance and health reporting every 60 seconds
        if (loop_counter >= 60) {
            loop_counter = 0;
            auto cameras = nvr::CameraManager::Instance().GetCameras();
            for (const auto& cam : cameras) {
                if (cam.enabled) {
                    bool online = nvr::CameraManager::Instance().IsCameraOnline(cam.id);
                    bool rec = nvr::RecordingScheduler::Instance().IsChannelRecording(cam.id);
                    uint32_t frames = nvr::RecordingScheduler::Instance().GetChannelFrameCount(cam.id);
                    uint64_t bytes = nvr::RecordingScheduler::Instance().GetChannelBytesWritten(cam.id);

                    LOG_INFO << "[Health] Cam " << cam.id << " (" << cam.name << "): Online: "
                             << (online ? "YES" : "NO") << " | REC: " << (rec ? "ACTIVE" : "STANDBY")
                             << " | Frames: " << frames << " | Bytes: " << (bytes / 1024) << " KB";
                }
            }
        }
    }

    LOG_INFO << "Shutting down NVR subsystems gracefully...";

    // 0. Stop HTTP REST Control Plane
    nvr::RestServer::Instance().Stop();

    // 1. Stop Live View Pipeline
    nvr::LiveController::Instance().Stop();

    // 2. Stop Camera Ingress
    nvr::CameraManager::Instance().StopAll();

    // 3. Flush and Finalize Recording Segments
    nvr::RecordingScheduler::Instance().StopAll();

    // 3. Stop Retention Manager
    nvr::RetentionManager::Instance().Stop();

    LOG_INFO << "NVR Core cleanly terminated. All segments finalized.";

#ifdef NVR_ENABLE_DEEP_MONITORING
    nvr::DeepTracer::Instance().Stop();
#endif

    return 0;
}
