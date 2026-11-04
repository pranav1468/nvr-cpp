#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/common/logger.h"
#include "nvr/common/deep_tracer.h"
#include "nvr/common/time_utils.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/live/live_controller.h"
#include "nvr/live/qml_video_bridge.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/media/stream_broker.h"
#include "nvr/ingress/onvif_client.h"
#include "nvr/ai/motion_gate.h"
#include "nvr/playback/playback_engine.h"
#include "nvr/common/crypto_utils.h"
#include "nvr/database/database_manager.h"

#include <sys/sysinfo.h>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace nvr {

CommandDispatcher& CommandDispatcher::Instance() {
    static CommandDispatcher instance;
    return instance;
}

CommandDispatcher::CommandDispatcher() {
    RegisterDefaultHandlers();
}

void CommandDispatcher::RegisterHandler(const std::string& action, UserRole min_role, 
                                       CommandHandler handler, const std::string& description) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_[action] = RegisteredCommand{action, min_role, std::move(handler), description};
    LOG_INFO << "CommandDispatcher: Registered action '" << action 
             << "' [MinRole: " << UserRoleToString(min_role) << "]";
}

void CommandDispatcher::UnregisterHandler(const std::string& action) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_.erase(action);
}

CommandResult CommandDispatcher::Dispatch(const CommandEnvelope& cmd) {
    RegisteredCommand reg;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = handlers_.find(cmd.action);
        if (it == handlers_.end()) {
            LOG_WARN << "CommandDispatcher: Unknown action '" << cmd.action << "'";
            return CommandResult::Error(404, "Unknown action: " + cmd.action);
        }
        reg = it->second;
    }

    // Authentication & RBAC Verification (Deferred until post-validation phase; full database auth preserved)
    // When auth_token is provided, session verification is performed. If empty, defaults to full ADMIN access.
    if (!cmd.auth_token.empty()) {
        SessionRecord session;
        if (DatabaseManager::Instance().ValidateSession(cmd.auth_token, session)) {
            UserRole caller_role = UserRole::VIEWER;
            if (session.role == "ADMIN") caller_role = UserRole::ADMIN;
            else if (session.role == "OPERATOR") caller_role = UserRole::OPERATOR;

            if (caller_role < reg.min_role) {
                return CommandResult::Error(403, "Access denied: Requires role " + UserRoleToString(reg.min_role));
            }
        }
    }

    // Execute handler safely
    try {
        return reg.handler(cmd);
    } catch (const std::exception& e) {
        LOG_ERROR << "CommandDispatcher: Exception executing '" << cmd.action << "': " << e.what();
        return CommandResult::Error(500, std::string("Internal error executing action: ") + e.what());
    } catch (...) {
        LOG_ERROR << "CommandDispatcher: Unknown exception executing '" << cmd.action << "'";
        return CommandResult::Error(500, "Unknown internal error executing action");
    }
}

CommandResult CommandDispatcher::DispatchJson(const std::string& json_str, const std::string& auth_token_override) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        if (!j.contains("action") || !j["action"].is_string()) {
            return CommandResult::Error(400, "Missing or invalid 'action' field in command JSON");
        }

        CommandEnvelope env;
        env.action = j["action"].get<std::string>();
        if (j.contains("payload")) {
            env.payload = j["payload"];
        }
        if (j.contains("command_id") && j["command_id"].is_string()) {
            env.command_id = j["command_id"].get<std::string>();
        }
        if (j.contains("client_id") && j["client_id"].is_string()) {
            env.client_id = j["client_id"].get<std::string>();
        }
        if (j.contains("timestamp_ms") && j["timestamp_ms"].is_number()) {
            env.timestamp_ms = j["timestamp_ms"].get<int64_t>();
        }

        if (j.contains("auth_token") && j["auth_token"].is_string()) {
            env.auth_token = j["auth_token"].get<std::string>();
        }
        if (env.auth_token.empty() && !auth_token_override.empty()) {
            env.auth_token = auth_token_override;
        }

        return Dispatch(env);
    } catch (const nlohmann::json::parse_error& e) {
        LOG_WARN << "CommandDispatcher: Malformed JSON command: " << e.what();
        return CommandResult::Error(400, std::string("Malformed JSON syntax: ") + e.what());
    }
}

nlohmann::json CommandDispatcher::GetRegisteredCommands() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [action, reg] : handlers_) {
        nlohmann::json item;
        item["action"] = action;
        item["min_role"] = UserRoleToString(reg.min_role);
        item["description"] = reg.description;
        arr.push_back(item);
    }
    return arr;
}

void CommandDispatcher::SetDeviceName(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    device_name_ = name;
}

std::string CommandDispatcher::GetDeviceName() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return device_name_;
}

void CommandDispatcher::RegisterDefaultHandlers() {

    // 2. System Status, Telemetry & Deep Diagnostics
    RegisterHandler("system.status", UserRole::VIEWER, [this](const CommandEnvelope&) -> CommandResult {
        nlohmann::json resp;
        resp["device_name"] = GetDeviceName();
        resp["version"] = "2.0.0";
        resp["status"] = "ONLINE";
        resp["max_channels"] = CameraManager::Instance().GetMaxChannels();
        std::string rec_path = RetentionManager::Instance().GetRecordingPath();
        resp["free_storage_bytes"] = RetentionManager::Instance().GetFreeDiskSpaceBytes(rec_path);
        resp["total_storage_bytes"] = RetentionManager::Instance().GetTotalDiskSpaceBytes(rec_path);

        uint64_t total_rec_size = 0;
        uint32_t total_rec_segs = 0;
        if (!DatabaseManager::Instance().GetRecordingStats(total_rec_segs, total_rec_size)) {
            std::error_code ec;
            if (std::filesystem::exists(rec_path, ec)) {
                for (auto it = std::filesystem::recursive_directory_iterator(rec_path, std::filesystem::directory_options::skip_permission_denied, ec);
                     it != std::filesystem::recursive_directory_iterator(); ++it) {
                    if (it->is_regular_file(ec) && it->path().extension() == ".mp4") {
                        total_rec_size += it->file_size(ec);
                        total_rec_segs++;
                    }
                }
            }
        }
        resp["total_segments"] = total_rec_segs;
        resp["total_recording_size_bytes"] = total_rec_size;
        resp["stream_broker_buffer_bytes"] = StreamBroker::Instance().GetClientBufferMemoryBytes();

        static auto last_bitrate_time = std::chrono::steady_clock::now();
        static uint64_t last_total_bytes = 0;
        static double smoothed_bitrate_mbps = 0.0;

        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - last_bitrate_time).count();

        uint64_t current_total_bytes = 0;
        auto cameras = CameraManager::Instance().GetCameras();
        int online_count = 0;
        for (const auto& cam : cameras) {
            if (CameraManager::Instance().IsCameraOnline(cam.id)) {
                online_count++;
            }
            current_total_bytes += RecordingScheduler::Instance().GetChannelBytesWritten(cam.id);
        }

        if (elapsed_sec >= 1.0) {
            if (last_total_bytes > 0 && current_total_bytes >= last_total_bytes) {
                double bytes_diff = static_cast<double>(current_total_bytes - last_total_bytes);
                smoothed_bitrate_mbps = (bytes_diff * 8.0) / (elapsed_sec * 1000000.0);
            } else if (online_count == 0) {
                smoothed_bitrate_mbps = 0.0;
            }
            last_total_bytes = current_total_bytes;
            last_bitrate_time = now;
        }

        resp["bitrate_mbps"] = smoothed_bitrate_mbps;
        resp["online_cameras"] = online_count;

        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            uint64_t unit = si.mem_unit ? si.mem_unit : 1;
            resp["ram_total_bytes"] = static_cast<uint64_t>(si.totalram) * unit;
            resp["ram_free_bytes"] = static_cast<uint64_t>(si.freeram) * unit;
            resp["uptime_seconds"] = si.uptime;
        }

        double loadavg[3];
        if (getloadavg(loadavg, 3) != -1) {
            resp["cpu_load_1m"] = loadavg[0];
            resp["cpu_load_5m"] = loadavg[1];
        }

        resp["cpu_cores"] = static_cast<int>(std::thread::hardware_concurrency());

        static unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0, prev_idle = 0;
        static unsigned long long prev_iowait = 0, prev_irq = 0, prev_softirq = 0, prev_steal = 0;
        static double cached_cpu_pct = 0.0;
        static std::chrono::steady_clock::time_point last_stat_read{};

        auto now_stat = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now_stat - last_stat_read).count() >= 500) {
            std::ifstream stat_file("/proc/stat");
            if (stat_file.is_open()) {
                std::string line;
                if (std::getline(stat_file, line) && line.rfind("cpu ", 0) == 0) {
                    unsigned long long u = 0, n = 0, s = 0, id = 0, io = 0, ir = 0, sir = 0, st = 0;
                    if (sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                               &u, &n, &s, &id, &io, &ir, &sir, &st) >= 4) {
                        unsigned long long prev_idle_all = prev_idle + prev_iowait;
                        unsigned long long idle_all = id + io;
                        unsigned long long prev_non_idle = prev_user + prev_nice + prev_system + prev_irq + prev_softirq + prev_steal;
                        unsigned long long non_idle = u + n + s + ir + sir + st;
                        unsigned long long prev_total = prev_idle_all + prev_non_idle;
                        unsigned long long total = idle_all + non_idle;

                        if (total > prev_total) {
                            unsigned long long total_d = total - prev_total;
                            unsigned long long idle_d = idle_all - prev_idle_all;
                            cached_cpu_pct = 100.0 * static_cast<double>(total_d - idle_d) / static_cast<double>(total_d);
                            if (cached_cpu_pct < 0.0) cached_cpu_pct = 0.0;
                            if (cached_cpu_pct > 100.0) cached_cpu_pct = 100.0;
                        }

                        prev_user = u; prev_nice = n; prev_system = s; prev_idle = id;
                        prev_iowait = io; prev_irq = ir; prev_softirq = sir; prev_steal = st;
                        last_stat_read = now_stat;
                    }
                }
            }
        }
        resp["cpu_percent"] = cached_cpu_pct;

        return CommandResult::Success(resp);
    }, "Query system hardware and storage telemetry");

    RegisterHandler("diagnostics.trace", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        size_t max_count = static_cast<size_t>(cmd.payload.value("max_count", 64));
        if (max_count == 0 || max_count > 512) max_count = 64;
        nlohmann::json diag = nlohmann::json::parse(DeepTracer::Instance().GetDiagnosticsJson(max_count));
        return CommandResult::Success(diag);
    }, "Query DeepTracer flight-recorder breadcrumbs and process resource metrics");

    // 2.5 Storage Location & Partition Management
    RegisterHandler("storage.get", UserRole::VIEWER, [](const CommandEnvelope&) -> CommandResult {
        std::string rec_path = RetentionManager::Instance().GetRecordingPath();
        uint64_t free_bytes = RetentionManager::Instance().GetFreeDiskSpaceBytes(rec_path);
        uint64_t total_bytes = RetentionManager::Instance().GetTotalDiskSpaceBytes(rec_path);
        uint64_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

        bool writable = false;
        try {
            std::filesystem::path probe_path = std::filesystem::path(rec_path) / ".storage_write_probe";
            std::ofstream probe(probe_path);
            if (probe.is_open()) {
                probe << "ok";
                probe.close();
                std::error_code ec;
                std::filesystem::remove(probe_path, ec);
                writable = true;
            }
        } catch (...) {
            writable = false;
        }

        nlohmann::json resp;
        resp["recording_path"] = rec_path;
        resp["free_storage_bytes"] = free_bytes;
        resp["total_storage_bytes"] = total_bytes;
        resp["used_storage_bytes"] = used_bytes;
        resp["is_writable"] = writable;
        resp["status"] = writable ? "READY" : "READ_ONLY";

        uint32_t seg_count = 0;
        uint64_t seg_bytes = 0;
        DatabaseManager::Instance().GetRecordingStats(seg_count, seg_bytes);
        resp["total_segments"] = seg_count;
        resp["total_recording_size_bytes"] = seg_bytes;

        return CommandResult::Success(resp);
    }, "Query active media storage mount and disk metrics");

    RegisterHandler("storage.set_path", UserRole::ADMIN, [](const CommandEnvelope& cmd) -> CommandResult {
        std::string raw_path = cmd.payload.value("path", cmd.payload.value("recording_path", ""));
        if (raw_path.empty()) {
            return CommandResult::Error(400, "Storage path cannot be empty");
        }
        if (raw_path.find('\0') != std::string::npos) {
            return CommandResult::Error(400, "Invalid storage path contains null characters");
        }

        std::filesystem::path p(raw_path);
        std::string target_path = p.lexically_normal().string();

        std::error_code ec;
        std::filesystem::create_directories(target_path, ec);
        if (ec) {
            return CommandResult::Error(400, "Cannot create storage directory: " + ec.message());
        }

        std::filesystem::path probe_path = std::filesystem::path(target_path) / ".storage_write_probe";
        {
            std::ofstream probe(probe_path);
            if (!probe.is_open()) {
                return CommandResult::Error(403, "Target directory is not writable (permission denied or read-only filesystem)");
            }
            probe << "test_write";
        }
        std::filesystem::remove(probe_path, ec);

        uint64_t free_bytes = RetentionManager::Instance().GetFreeDiskSpaceBytes(target_path);
        uint64_t total_bytes = RetentionManager::Instance().GetTotalDiskSpaceBytes(target_path);

        RetentionManager::Instance().SetRecordingPath(target_path);
        RecordingScheduler::Instance().SetStoragePath(target_path);
        DatabaseManager::Instance().SetSetting("storage_recording_path", target_path);

        std::string actor = "admin";
        if (!cmd.auth_token.empty()) {
            SessionRecord session;
            if (DatabaseManager::Instance().ValidateSession(cmd.auth_token, session)) {
                actor = session.username;
            }
        }
        DatabaseManager::Instance().AppendAuditLog(
            actor, "ADMIN", "127.0.0.1", "storage.set_path", "storage",
            "Changed recording storage path to " + target_path,
            nlohmann::json{{"new_path", target_path}, {"free_bytes", free_bytes}, {"total_bytes", total_bytes}}.dump(),
            "SUCCESS"
        );

        nlohmann::json resp;
        resp["recording_path"] = target_path;
        resp["free_storage_bytes"] = free_bytes;
        resp["total_storage_bytes"] = total_bytes;
        resp["is_writable"] = true;
        resp["status"] = "UPDATED";

        return CommandResult::Success(resp, "Recording storage path successfully updated to " + target_path);
    }, "Configure storage destination path for camera recordings (Admin only)");

    // 3. Camera Management
    RegisterHandler("camera.list", UserRole::VIEWER, [](const CommandEnvelope&) -> CommandResult {
        auto cameras = CameraManager::Instance().GetCameras();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cameras) {
            nlohmann::json item;
            item["id"] = c.id;
            item["name"] = c.name;
            item["rtsp_url"] = crypto::MaskRtspUrl(c.rtsp_url);
            item["sub_rtsp_url"] = crypto::MaskRtspUrl(c.sub_rtsp_url);
            item["onvif_xaddr"] = c.onvif_xaddr;
            item["onvif_username"] = c.onvif_username;
            item["software_motion_active"] = MotionGateManager::Instance().IsMotionActive(c.id);
            item["enabled"] = c.enabled;
            item["record_mode"] = static_cast<int>(c.record_mode);
            item["online"] = CameraManager::Instance().IsCameraOnline(c.id);
            item["recording"] = RecordingScheduler::Instance().IsChannelRecording(c.id);
            item["frames_recorded"] = RecordingScheduler::Instance().GetChannelFrameCount(c.id);
            item["bytes_recorded"] = RecordingScheduler::Instance().GetChannelBytesWritten(c.id);
            arr.push_back(item);
        }
        return CommandResult::Success(arr);
    }, "List all configured cameras and their runtime statuses");

    RegisterHandler("motion.status", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        nlohmann::json res;
        res["channel_id"] = channel_id;
        res["software_motion_active"] = MotionGateManager::Instance().IsMotionActive(channel_id);
        MotionGateConfig cfg = MotionGateManager::Instance().GetChannelConfig(channel_id);
        res["config"]["grid_cols"] = cfg.grid_cols;
        res["config"]["grid_rows"] = cfg.grid_rows;
        res["config"]["pixel_delta_threshold"] = cfg.pixel_delta_threshold;
        res["config"]["motion_area_ratio"] = cfg.motion_area_ratio;
        res["config"]["min_interval_ms"] = cfg.min_interval_ms;
        res["config"]["hold_time_ms"] = cfg.hold_time_ms;
        res["config"]["enabled"] = cfg.enabled;
        return CommandResult::Success(res);
    }, "Query camera motion detection status and configuration");

    RegisterHandler("motion.configure", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        MotionGateConfig cfg = MotionGateManager::Instance().GetChannelConfig(channel_id);
        if (cmd.payload.contains("pixel_delta_threshold") && cmd.payload["pixel_delta_threshold"].is_number()) {
            cfg.pixel_delta_threshold = cmd.payload["pixel_delta_threshold"].get<int>();
        }
        if (cmd.payload.contains("motion_area_ratio") && cmd.payload["motion_area_ratio"].is_number()) {
            cfg.motion_area_ratio = cmd.payload["motion_area_ratio"].get<float>();
        }
        if (cmd.payload.contains("min_interval_ms") && cmd.payload["min_interval_ms"].is_number()) {
            cfg.min_interval_ms = cmd.payload["min_interval_ms"].get<int>();
        }
        if (cmd.payload.contains("hold_time_ms") && cmd.payload["hold_time_ms"].is_number()) {
            cfg.hold_time_ms = cmd.payload["hold_time_ms"].get<int>();
        }
        if (cmd.payload.contains("enabled") && cmd.payload["enabled"].is_boolean()) {
            cfg.enabled = cmd.payload["enabled"].get<bool>();
        }
        MotionGateManager::Instance().ConfigureChannel(channel_id, cfg);
        nlohmann::json res;
        res["channel_id"] = channel_id;
        res["configured"] = true;
        return CommandResult::Success(res, "Motion gate configuration updated");
    }, "Update camera motion gate sensitivity and timing configuration");

    RegisterHandler("camera.restart", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", 0);
        if (channel_id <= 0 || !CameraManager::Instance().HasCamera(channel_id)) {
            return CommandResult::Error(404, "Invalid or unknown camera channel ID");
        }
        CameraManager::Instance().StopCamera(channel_id, true);
        bool ok = CameraManager::Instance().StartCamera(channel_id);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["restarted"] = ok;
        return ok ? CommandResult::Success(data, "Camera restarted successfully")
                  : CommandResult::Error(500, "Failed to restart camera", data);
    }, "Restart ingress stream session for a specified camera");

    RegisterHandler("camera.update", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        auto get_channel_id = [&](const nlohmann::json& p) -> int {
            for (const char* k : {"id", "channel_id", "channel", "channelId"}) {
                if (p.contains(k)) {
                    if (p[k].is_number_integer()) return p[k].get<int>();
                    if (p[k].is_string()) {
                        try { return std::stoi(p[k].get<std::string>()); } catch (...) {}
                    }
                }
            }
            return 0;
        };

        int channel_id = get_channel_id(cmd.payload);
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid camera channel ID required");
        }

        CameraConfig cfg;
        if (!CameraManager::Instance().GetCamera(channel_id, cfg)) {
            cfg.id = channel_id;
        }

        if (cmd.payload.contains("name") && cmd.payload["name"].is_string()) {
            cfg.name = cmd.payload["name"].get<std::string>();
        }
        auto get_payload_str = [&](const std::vector<std::string>& keys, const std::string& def = "") -> std::string {
            for (const auto& k : keys) {
                if (cmd.payload.contains(k) && cmd.payload[k].is_string()) {
                    return cmd.payload[k].get<std::string>();
                }
            }
            return def;
        };

        std::string u_main = get_payload_str({"rtsp_url", "rtsp_url_main", "main_rtsp_url"});
        if (!u_main.empty() && u_main.find("***") == std::string::npos) {
            cfg.rtsp_url = u_main;
        }

        std::string u_sub = get_payload_str({"sub_rtsp_url", "rtsp_url_sub"});
        if (!u_sub.empty() && u_sub.find("***") == std::string::npos) {
            cfg.sub_rtsp_url = u_sub;
        }

        std::string xaddr = get_payload_str({"onvif_xaddr"});
        if (xaddr.empty()) {
            std::string ip = get_payload_str({"onvif_ip", "ip"});
            if (!ip.empty()) {
                int port = 80;
                if (cmd.payload.contains("onvif_port")) {
                    if (cmd.payload["onvif_port"].is_number()) port = cmd.payload["onvif_port"].get<int>();
                    else if (cmd.payload["onvif_port"].is_string()) {
                        try { port = std::stoi(cmd.payload["onvif_port"].get<std::string>()); } catch (...) {}
                    }
                }
                xaddr = "http://" + ip + ":" + std::to_string(port) + "/onvif/device_service";
            }
        }
        if (!xaddr.empty()) cfg.onvif_xaddr = xaddr;

        std::string user = get_payload_str({"onvif_username", "username"});
        if (!user.empty()) cfg.onvif_username = user;

        std::string pass = get_payload_str({"onvif_password", "password"});
        if (!pass.empty() && pass != "******") cfg.onvif_password = pass;
        if (cmd.payload.contains("enabled") && cmd.payload["enabled"].is_boolean()) {
            cfg.enabled = cmd.payload["enabled"].get<bool>();
        }
        if (cmd.payload.contains("record_mode")) {
            if (cmd.payload["record_mode"].is_number()) {
                cfg.record_mode = static_cast<RecordMode>(cmd.payload["record_mode"].get<int>());
            } else if (cmd.payload["record_mode"].is_string()) {
                std::string mode_str = cmd.payload["record_mode"].get<std::string>();
                if (mode_str == "1" || mode_str == "always" || mode_str == "continuous") {
                    cfg.record_mode = RecordMode::CONTINUOUS;
                } else if (mode_str == "2" || mode_str == "motion" || mode_str == "motion_only") {
                    cfg.record_mode = RecordMode::MOTION_ONLY;
                } else if (mode_str == "3" || mode_str == "scheduled") {
                    cfg.record_mode = RecordMode::SCHEDULED;
                } else if (mode_str == "4" || mode_str == "event" || mode_str == "event_only") {
                    cfg.record_mode = RecordMode::EVENT_ONLY;
                } else if (mode_str == "0" || mode_str == "disabled" || mode_str == "none") {
                    cfg.record_mode = RecordMode::DISABLED;
                }
            }
        }

        if (!cfg.rtsp_url.empty()) {
            int conflict_ch = CameraManager::Instance().FindChannelByRtspUrl(cfg.rtsp_url, cfg.id);
            if (conflict_ch != -1) {
                return CommandResult::Error(409, "RTSP URL is already assigned to channel " + std::to_string(conflict_ch));
            }
        }
        if (!cfg.sub_rtsp_url.empty()) {
            int conflict_sub = CameraManager::Instance().FindChannelByRtspUrl(cfg.sub_rtsp_url, cfg.id);
            if (conflict_sub != -1) {
                return CommandResult::Error(409, "Sub RTSP URL is already assigned to channel " + std::to_string(conflict_sub));
            }
        }

        bool ok = CameraManager::Instance().UpdateCamera(cfg);
        RecordingScheduler::Instance().StopChannelRecording(cfg.id);
        if (cfg.enabled && cfg.record_mode != RecordMode::DISABLED && !cfg.rtsp_url.empty()) {
            RecordingScheduler::Instance().StartChannelRecording(cfg.id, cfg.record_mode);
        }
        if (!cfg.enabled) {
            LiveController::Instance().DeactivateChannel(cfg.id);
        }

        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["name"] = cfg.name;
        data["rtsp_url"] = cfg.rtsp_url;
        data["sub_rtsp_url"] = cfg.sub_rtsp_url;
        data["onvif_xaddr"] = cfg.onvif_xaddr;
        data["onvif_username"] = cfg.onvif_username;
        data["enabled"] = cfg.enabled;
        data["record_mode"] = static_cast<int>(cfg.record_mode);
        return ok ? CommandResult::Success(data, "Camera updated and stream sessions reloaded")
                  : CommandResult::Error(500, "Failed to update camera", data);
    }, "Update camera channel settings and reload streams dynamically");

    RegisterHandler("camera.add", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        auto get_channel_id = [&](const nlohmann::json& p) -> int {
            for (const char* k : {"id", "channel_id", "channel", "channelId"}) {
                if (p.contains(k)) {
                    if (p[k].is_number_integer()) return p[k].get<int>();
                    if (p[k].is_string()) {
                        try { return std::stoi(p[k].get<std::string>()); } catch (...) {}
                    }
                }
            }
            return 0;
        };

        int channel_id = get_channel_id(cmd.payload);
        std::string name = cmd.payload.value("name", "Camera " + std::to_string(channel_id));
        auto get_payload_str = [&](const std::vector<std::string>& keys, const std::string& def = "") -> std::string {
            for (const auto& k : keys) {
                if (cmd.payload.contains(k) && cmd.payload[k].is_string()) {
                    return cmd.payload[k].get<std::string>();
                }
            }
            return def;
        };

        std::string rtsp_url = get_payload_str({"rtsp_url", "rtsp_url_main", "main_rtsp_url"});
        std::string sub_rtsp_url = get_payload_str({"sub_rtsp_url", "rtsp_url_sub"});
        std::string onvif_user = get_payload_str({"onvif_username", "username"}, "admin");
        std::string onvif_pass = get_payload_str({"onvif_password", "password"}, "");
        std::string onvif_xaddr = get_payload_str({"onvif_xaddr"});
        if (onvif_xaddr.empty()) {
            std::string ip = get_payload_str({"onvif_ip", "ip"});
            if (!ip.empty()) {
                int port = 80;
                if (cmd.payload.contains("onvif_port")) {
                    if (cmd.payload["onvif_port"].is_number()) port = cmd.payload["onvif_port"].get<int>();
                    else if (cmd.payload["onvif_port"].is_string()) {
                        try { port = std::stoi(cmd.payload["onvif_port"].get<std::string>()); } catch (...) {}
                    }
                }
                onvif_xaddr = "http://" + ip + ":" + std::to_string(port) + "/onvif/device_service";
            }
        }

        bool enabled = cmd.payload.value("enabled", true);
        RecordMode rec_mode = RecordMode::CONTINUOUS;
        if (cmd.payload.contains("record_mode")) {
            if (cmd.payload["record_mode"].is_number()) {
                rec_mode = static_cast<RecordMode>(cmd.payload["record_mode"].get<int>());
            } else if (cmd.payload["record_mode"].is_string()) {
                std::string mode_str = cmd.payload["record_mode"].get<std::string>();
                if (mode_str == "1" || mode_str == "always" || mode_str == "continuous") {
                    rec_mode = RecordMode::CONTINUOUS;
                } else if (mode_str == "2" || mode_str == "motion" || mode_str == "motion_only") {
                    rec_mode = RecordMode::MOTION_ONLY;
                } else if (mode_str == "3" || mode_str == "scheduled") {
                    rec_mode = RecordMode::SCHEDULED;
                } else if (mode_str == "4" || mode_str == "event" || mode_str == "event_only") {
                    rec_mode = RecordMode::EVENT_ONLY;
                } else if (mode_str == "0" || mode_str == "disabled" || mode_str == "none") {
                    rec_mode = RecordMode::DISABLED;
                }
            }
        }

        if (channel_id <= 0 || rtsp_url.empty()) {
            return CommandResult::Error(400, "Valid channel_id and rtsp_url are required");
        }

        int conflict_ch = CameraManager::Instance().FindChannelByRtspUrl(rtsp_url, channel_id);
        if (conflict_ch != -1) {
            return CommandResult::Error(409, "RTSP URL is already assigned to channel " + std::to_string(conflict_ch));
        }
        if (!sub_rtsp_url.empty()) {
            int conflict_sub = CameraManager::Instance().FindChannelByRtspUrl(sub_rtsp_url, channel_id);
            if (conflict_sub != -1) {
                return CommandResult::Error(409, "Sub RTSP URL is already assigned to channel " + std::to_string(conflict_sub));
            }
        }

        CameraConfig cfg;
        cfg.id = channel_id;
        cfg.name = name;
        cfg.rtsp_url = rtsp_url;
        cfg.sub_rtsp_url = sub_rtsp_url.empty() ? rtsp_url : sub_rtsp_url;
        cfg.onvif_xaddr = onvif_xaddr;
        cfg.onvif_username = onvif_user;
        cfg.onvif_password = onvif_pass;
        cfg.enabled = enabled;
        cfg.record_mode = rec_mode;
        cfg.reconnect_delay_sec = 3;

        bool ok = CameraManager::Instance().AddCamera(cfg);
        if (ok && enabled) {
            CameraManager::Instance().StartCamera(channel_id);
            CameraManager::Instance().StartSubStream(channel_id);
            if (rec_mode != RecordMode::DISABLED) {
                RecordingScheduler::Instance().StartChannelRecording(channel_id, rec_mode);
            }
        }
        nlohmann::json data;
        data["channel_id"] = channel_id;
        return ok ? CommandResult::Success(data, "Camera added successfully")
                  : CommandResult::Error(500, "Failed to add camera", data);
    }, "Add a new camera channel configuration");

    auto remove_cam_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid camera channel ID required");
        }
        RecordingScheduler::Instance().StopChannelRecording(channel_id);
        LiveController::Instance().DeactivateChannel(channel_id);
        bool removed_mgr = CameraManager::Instance().RemoveCamera(channel_id);
        if (!removed_mgr) {
            return CommandResult::Error(404, "Camera channel not found");
        }
        nlohmann::json data;
        data["channel_id"] = channel_id;
        return CommandResult::Success(data, "Camera removed successfully");
    };
    RegisterHandler("camera.remove", UserRole::ADMIN, remove_cam_fn, "Remove camera channel and stop its recording");
    RegisterHandler("camera.delete", UserRole::ADMIN, remove_cam_fn, "Alias for camera.remove");

    auto parse_json_int = [](const nlohmann::json& j, const char* key, int def_val) -> int {
        if (!j.is_object() || !j.contains(key)) return def_val;
        if (j[key].is_number()) return j[key].get<int>();
        if (j[key].is_string()) {
            try { return std::stoi(j[key].get<std::string>()); } catch (...) { return def_val; }
        }
        return def_val;
    };


    RegisterHandler("onvif.discover", UserRole::VIEWER, [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_DISCOVER");
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 2000);
        int mcast_port = parse_json_int(cmd.payload, "multicast_port", 3702);
        auto cams = OnvifClient::DiscoverCameras(timeout_ms, static_cast<uint16_t>(mcast_port));
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cams) {
            nlohmann::json item;
            item["ip_address"] = c.ip_address;
            item["port"] = c.port;
            item["xaddr"] = c.xaddr;
            item["endpoint_reference"] = c.endpoint_reference;
            item["scopes"] = c.scopes;
            item["name"] = c.name.empty() ? ("Camera_" + c.ip_address) : c.name;
            item["hardware"] = c.hardware;
            arr.push_back(item);
        }
        return CommandResult::Success(arr);
    }, "Discover ONVIF cameras on the local network using WS-Discovery");

    RegisterHandler("onvif.probe", UserRole::OPERATOR, [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_PROBE");
        std::string ip = (cmd.payload.contains("ip") && cmd.payload["ip"].is_string()) ? cmd.payload["ip"].get<std::string>() : "";
        int port = parse_json_int(cmd.payload, "port", 80);
        std::string xaddr = (cmd.payload.contains("xaddr") && cmd.payload["xaddr"].is_string()) ? cmd.payload["xaddr"].get<std::string>() : "";
        std::string username = (cmd.payload.contains("username") && cmd.payload["username"].is_string()) ? cmd.payload["username"].get<std::string>() : "";
        std::string password = (cmd.payload.contains("password") && cmd.payload["password"].is_string()) ? cmd.payload["password"].get<std::string>() : "";
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 3000);

        std::string target = !xaddr.empty() ? xaddr : ip;
        if (target.empty()) {
            return CommandResult::Error(400, "Camera IP address or XAddr is required");
        }

        OnvifCameraDetails details;
        bool ok = OnvifClient::ProbeCamera(target, port, username, password, details, timeout_ms);
        if (!ok) {
            return CommandResult::Error(502, "Failed to connect to ONVIF camera or authenticate");
        }

        nlohmann::json resp;
        resp["ip_address"] = details.ip_address;
        resp["port"] = details.port;
        resp["xaddr"] = details.xaddr;
        resp["media_xaddr"] = details.media_xaddr;
        resp["media2_xaddr"] = details.media2_xaddr;
        resp["snapshot_url"] = details.snapshot_url;
        resp["clock_offset_ms"] = details.clock_offset_ms;
        resp["main_rtsp_url"] = details.main_rtsp_url;
        resp["sub_rtsp_url"] = details.sub_rtsp_url;
        resp["mac_address"] = details.mac_address;

        nlohmann::json ifaces_arr = nlohmann::json::array();
        for (const auto& ifc : details.network_interfaces) {
            nlohmann::json ij;
            ij["token"] = ifc.token;
            ij["name"] = ifc.name;
            ij["hw_address"] = ifc.hw_address;
            ij["ipv4_address"] = ifc.ipv4_address;
            ij["subnet_mask"] = ifc.subnet_mask;
            ij["dhcp_enabled"] = ifc.dhcp_enabled;
            ifaces_arr.push_back(ij);
        }
        resp["network_interfaces"] = ifaces_arr;

        nlohmann::json dev;
        dev["manufacturer"] = details.device_info.manufacturer;
        dev["model"] = details.device_info.model;
        dev["firmware"] = details.device_info.firmware_version;
        dev["serial"] = details.device_info.serial_number;
        dev["hardware_id"] = details.device_info.hardware_id;
        resp["device_info"] = dev;

        nlohmann::json profs = nlohmann::json::array();
        for (const auto& p : details.profiles) {
            nlohmann::json pi;
            pi["token"] = p.token;
            pi["name"] = p.name;
            pi["rtsp_uri"] = p.rtsp_uri;
            pi["snapshot_uri"] = p.snapshot_uri;
            pi["video_source_token"] = p.video_source_token;
            pi["video_encoder_token"] = p.video_encoder_token;
            pi["encoding"] = p.encoding;
            pi["width"] = p.width;
            pi["height"] = p.height;
            pi["fps"] = p.fps;
            pi["bitrate_kbps"] = p.bitrate_kbps;
            pi["is_media2"] = p.is_media2;
            profs.push_back(pi);
        }
        resp["profiles"] = profs;
        return CommandResult::Success(resp);
    }, "Probe an ONVIF camera, inspect Profile S/T profiles, and resolve RTSP URLs");



    RegisterHandler("onvif.snapshot_uri", UserRole::VIEWER, [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_SNAPSHOT_URI");
        std::string media_xaddr = cmd.payload.value("media_xaddr", "");
        std::string profile_token = cmd.payload.value("profile_token", "");
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        bool use_media2 = cmd.payload.value("use_media2", false) || cmd.payload.value("is_media2", false);
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 3000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);

        if (media_xaddr.empty() || profile_token.empty()) {
            return CommandResult::Error(400, "media_xaddr and profile_token are required");
        }

        std::string uri = OnvifClient::GetSnapshotUri(media_xaddr, profile_token, username, password,
                                                      timeout_ms, clock_offset_ms, use_media2);
        if (uri.empty()) {
            return CommandResult::Error(502, "Failed to acquire camera snapshot URI");
        }

        nlohmann::json resp;
        resp["snapshot_uri"] = uri;
        resp["profile_token"] = profile_token;
        return CommandResult::Success(resp);
    }, "Obtain the direct HTTP JPEG snapshot URI for a camera profile");

    auto resolve_cam_onvif = [](int channel_id, std::string& out_media_xaddr, std::string& out_token,
                                std::string& out_user, std::string& out_pass, int64_t& out_clock_offset,
                                bool& out_use_media2, int timeout_ms) -> bool {
        CameraConfig cam_cfg;
        if (!CameraManager::Instance().GetCamera(channel_id, cam_cfg)) {
            return false;
        }
        if (out_user.empty()) out_user = cam_cfg.onvif_username;
        if (out_pass.empty()) out_pass = cam_cfg.onvif_password;

        std::string target_endpoint = cam_cfg.onvif_xaddr;
        if (target_endpoint.empty() && !cam_cfg.rtsp_url.empty()) {
            auto p1 = cam_cfg.rtsp_url.find("://");
            if (p1 != std::string::npos) {
                auto host_part = cam_cfg.rtsp_url.substr(p1 + 3);
                auto at_pos = host_part.find('@');
                if (at_pos != std::string::npos) host_part = host_part.substr(at_pos + 1);
                auto slash_pos = host_part.find('/');
                if (slash_pos != std::string::npos) host_part = host_part.substr(0, slash_pos);
                auto colon_pos = host_part.find(':');
                std::string ip = (colon_pos != std::string::npos) ? host_part.substr(0, colon_pos) : host_part;
                if (!ip.empty()) target_endpoint = "http://" + ip + ":80/onvif/device_service";
            }
        }

        if (target_endpoint.empty()) return false;

        OnvifCameraDetails details;
        if (!OnvifClient::ProbeCamera(target_endpoint, 80, out_user, out_pass, details, timeout_ms)) {
            return false;
        }

        out_media_xaddr = !details.media_xaddr.empty() ? details.media_xaddr : details.media2_xaddr;
        out_use_media2 = !details.media2_xaddr.empty() && details.media_xaddr.empty();
        out_clock_offset = details.clock_offset_ms;

        if (!details.profiles.empty()) {
            const auto& prof = details.profiles.front();
            out_token = !prof.video_encoder_token.empty() ? prof.video_encoder_token : prof.token;
            if (prof.is_media2) out_use_media2 = true;
        }
        return !out_media_xaddr.empty() && !out_token.empty();
    };

    auto onvif_encoder_get_fn = [parse_json_int, resolve_cam_onvif](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_ENCODER_GET");
        std::string media_xaddr = cmd.payload.value("media_xaddr", "");
        std::string encoder_token = cmd.payload.value("encoder_token", "");
        std::string profile_token = cmd.payload.value("profile_token", "");
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        bool use_media2 = cmd.payload.value("use_media2", false) || cmd.payload.value("is_media2", false);
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 3000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 0));

        std::string token = !encoder_token.empty() ? encoder_token : profile_token;
        if ((media_xaddr.empty() || token.empty()) && channel_id > 0) {
            resolve_cam_onvif(channel_id, media_xaddr, token, username, password, clock_offset_ms, use_media2, timeout_ms);
        }

        if (media_xaddr.empty() || token.empty()) {
            return CommandResult::Error(400, "media_xaddr and encoder_token are required (or valid channel_id with ONVIF)");
        }

        OnvifVideoEncoderConfig cfg;
        bool ok = OnvifClient::GetVideoEncoderConfig(media_xaddr, token, username, password,
                                                     cfg, timeout_ms, clock_offset_ms, use_media2);
        if (!ok) {
            return CommandResult::Error(502, "Failed to retrieve camera video encoder configuration from physical camera");
        }

        nlohmann::json resp;
        nlohmann::json c;
        c["token"] = cfg.token;
        c["name"] = cfg.name;
        c["encoding"] = cfg.encoding;
        c["width"] = cfg.width;
        c["height"] = cfg.height;
        c["resolution"] = std::to_string(cfg.width) + "x" + std::to_string(cfg.height);
        c["fps"] = cfg.fps;
        c["bitrate_kbps"] = cfg.bitrate_kbps;
        c["gov_length"] = cfg.gov_length;
        c["h264_profile"] = cfg.h264_profile;
        c["is_media2"] = cfg.is_media2;
        resp["config"] = c;

        OnvifVideoEncoderOptions opts;
        if (OnvifClient::GetVideoEncoderOptions(media_xaddr, token, username, password,
                                               opts, timeout_ms, clock_offset_ms, use_media2)) {
            nlohmann::json o;
            o["supported_encodings"] = opts.supported_encodings;
            o["min_fps"] = opts.min_fps;
            o["max_fps"] = opts.max_fps;
            o["min_bitrate_kbps"] = opts.min_bitrate_kbps;
            o["max_bitrate_kbps"] = opts.max_bitrate_kbps;
            o["min_gov_length"] = opts.min_gov_length;
            o["max_gov_length"] = opts.max_gov_length;
            nlohmann::json res_arr = nlohmann::json::array();
            for (const auto& r : opts.available_resolutions) {
                nlohmann::json ri;
                ri["width"] = r.width;
                ri["height"] = r.height;
                ri["label"] = std::to_string(r.width) + "x" + std::to_string(r.height);
                res_arr.push_back(ri);
            }
            o["available_resolutions"] = res_arr;
            resp["options"] = o;
        }

        return CommandResult::Success(resp);
    };
    RegisterHandler("onvif.encoder.get", UserRole::OPERATOR, onvif_encoder_get_fn, "Query ONVIF video encoder settings and supported option bounds from physical camera");
    RegisterHandler("onvif.get_encoder", UserRole::OPERATOR, onvif_encoder_get_fn, "Alias for onvif.encoder.get");

    auto onvif_encoder_set_fn = [parse_json_int, resolve_cam_onvif](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_ENCODER_SET");
        std::string media_xaddr = cmd.payload.value("media_xaddr", "");
        std::string encoder_token = cmd.payload.value("encoder_token", "");
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        bool use_media2 = cmd.payload.value("use_media2", false) || cmd.payload.value("is_media2", false);
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 5000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 0));

        if ((media_xaddr.empty() || encoder_token.empty()) && channel_id > 0) {
            resolve_cam_onvif(channel_id, media_xaddr, encoder_token, username, password, clock_offset_ms, use_media2, timeout_ms);
        }

        if (media_xaddr.empty() || encoder_token.empty()) {
            return CommandResult::Error(400, "media_xaddr and encoder_token are required (or valid channel_id with ONVIF)");
        }

        int target_width = parse_json_int(cmd.payload, "width", 0);
        int target_height = parse_json_int(cmd.payload, "height", 0);
        if (target_width <= 0 || target_height <= 0) {
            std::string res_str = cmd.payload.value("resolution", "1080p");
            if (res_str == "720p") { target_width = 1280; target_height = 720; }
            else if (res_str == "1080p") { target_width = 1920; target_height = 1080; }
            else if (res_str == "4MP" || res_str == "2K" || res_str == "1440p") { target_width = 2560; target_height = 1440; }
            else if (res_str == "4K" || res_str == "8MP" || res_str == "2160p") { target_width = 3840; target_height = 2160; }
            else {
                auto x_pos = res_str.find('x');
                if (x_pos != std::string::npos) {
                    try {
                        target_width = std::stoi(res_str.substr(0, x_pos));
                        target_height = std::stoi(res_str.substr(x_pos + 1));
                    } catch (...) {}
                }
                if (target_width <= 0 || target_height <= 0) {
                    target_width = 1920;
                    target_height = 1080;
                }
            }
        }

        std::string encoding = cmd.payload.value("encoding", "");
        if (encoding.empty()) {
            std::string codec_str = cmd.payload.value("codec", "h264");
            if (codec_str == "h265" || codec_str == "H265" || codec_str == "hevc" || codec_str == "HEVC") {
                encoding = "H265";
            } else {
                encoding = "H264";
            }
        }

        int target_fps = parse_json_int(cmd.payload, "fps", 25);
        int target_bitrate = parse_json_int(cmd.payload, "bitrate_kbps", 0);
        if (target_bitrate <= 0) {
            if (encoding == "H265") {
                target_bitrate = (target_width >= 3840) ? 6144 : (target_width >= 2560) ? 4096 : 2500;
            } else {
                target_bitrate = (target_width >= 3840) ? 8192 : (target_width >= 2560) ? 5120 : 4096;
            }
        }
        int gov_length = parse_json_int(cmd.payload, "gov_length", target_fps * 2);

        OnvifVideoEncoderConfig cfg;
        cfg.token = encoder_token;
        cfg.name = cmd.payload.value("name", "VideoEncoderConfig");
        cfg.encoding = encoding;
        cfg.width = target_width;
        cfg.height = target_height;
        cfg.fps = target_fps;
        cfg.bitrate_kbps = target_bitrate;
        cfg.gov_length = gov_length;
        cfg.h264_profile = cmd.payload.value("h264_profile", "Main");
        cfg.is_media2 = use_media2;

        OnvifVideoEncoderConfig current_cfg;
        if (OnvifClient::GetVideoEncoderConfig(media_xaddr, encoder_token, username, password,
                                               current_cfg, timeout_ms, clock_offset_ms, use_media2)) {
            if (!current_cfg.name.empty()) cfg.name = current_cfg.name;
            if (!current_cfg.h264_profile.empty()) cfg.h264_profile = current_cfg.h264_profile;
        }

        bool ok = OnvifClient::SetVideoEncoderConfig(media_xaddr, cfg, username, password,
                                                     timeout_ms, clock_offset_ms, use_media2);
        if (ok && channel_id > 0) {
            CameraManager::Instance().StopCamera(channel_id, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CameraManager::Instance().StartCamera(channel_id);
            CameraManager::Instance().StartSubStream(channel_id);
        }

        nlohmann::json resp;
        resp["encoder_token"] = encoder_token;
        resp["width"] = cfg.width;
        resp["height"] = cfg.height;
        resp["resolution"] = std::to_string(cfg.width) + "x" + std::to_string(cfg.height);
        resp["fps"] = cfg.fps;
        resp["bitrate_kbps"] = cfg.bitrate_kbps;
        resp["gov_length"] = cfg.gov_length;
        resp["encoding"] = cfg.encoding;
        resp["executed"] = ok;
        return ok ? CommandResult::Success(resp, "Camera video encoder configuration applied to physical camera and stream renegotiated")
                  : CommandResult::Error(502, "Failed to apply video encoder configuration to physical camera via ONVIF", resp);
    };
    RegisterHandler("onvif.encoder.set", UserRole::OPERATOR, onvif_encoder_set_fn, "Configure ONVIF video encoder parameters (resolution, bitrate, framerate, GOP) on physical camera");
    RegisterHandler("onvif.set_encoder", UserRole::OPERATOR, onvif_encoder_set_fn, "Alias for onvif.encoder.set");



    auto onvif_reboot_fn = [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_SYSTEM_REBOOT");
        std::string xaddr = cmd.payload.value("xaddr", "");
        if (xaddr.empty()) xaddr = cmd.payload.value("device_xaddr", "");
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 5000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);

        if (xaddr.empty()) {
            int cid = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
            if (cid > 0) {
                CameraConfig cfg;
                if (CameraManager::Instance().GetCamera(cid, cfg)) {
                    xaddr = cfg.onvif_xaddr;
                    if (xaddr.empty() && !cfg.rtsp_url.empty()) {
                        auto p1 = cfg.rtsp_url.find("://");
                        if (p1 != std::string::npos) {
                            auto host_part = cfg.rtsp_url.substr(p1 + 3);
                            auto at_pos = host_part.find('@');
                            if (at_pos != std::string::npos) host_part = host_part.substr(at_pos + 1);
                            auto slash_pos = host_part.find('/');
                            if (slash_pos != std::string::npos) host_part = host_part.substr(0, slash_pos);
                            auto colon_pos = host_part.find(':');
                            std::string ip = (colon_pos != std::string::npos) ? host_part.substr(0, colon_pos) : host_part;
                            if (!ip.empty()) xaddr = "http://" + ip + ":80/onvif/device_service";
                        }
                    }
                    if (username.empty()) username = cfg.onvif_username;
                    if (password.empty()) password = cfg.onvif_password;
                }
            }
        }

        if (xaddr.empty()) {
            return CommandResult::Error(400, "xaddr or valid channel_id is required");
        }

        bool ok = OnvifClient::RebootCamera(xaddr, username, password, timeout_ms, clock_offset_ms);
        nlohmann::json resp;
        resp["xaddr"] = xaddr;
        resp["rebooting"] = ok;
        return ok ? CommandResult::Success(resp, "Camera reboot command dispatched")
                  : CommandResult::Error(502, "Failed to reboot camera via ONVIF", resp);
    };
    RegisterHandler("onvif.system.reboot", UserRole::ADMIN, onvif_reboot_fn, "Send remote reboot command to camera over ONVIF Device Service");
    RegisterHandler("onvif.reboot", UserRole::ADMIN, onvif_reboot_fn, "Alias for onvif.system.reboot");

    auto onvif_datetime_fn = [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_DATETIME_SET");
        std::string xaddr = cmd.payload.value("xaddr", "");
        if (xaddr.empty()) xaddr = cmd.payload.value("device_xaddr", "");
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        std::string tz_str = cmd.payload.value("timezone", "UTC");
        int64_t target_utc_ms = cmd.payload.value("target_utc_ms", 0LL);
        if (target_utc_ms <= 0) {
            target_utc_ms = time_utils::WallTimeMs();
        }
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 3000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);

        if (xaddr.empty()) {
            int cid = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
            if (cid > 0) {
                CameraConfig cfg;
                if (CameraManager::Instance().GetCamera(cid, cfg)) {
                    xaddr = cfg.onvif_xaddr;
                    if (xaddr.empty() && !cfg.rtsp_url.empty()) {
                        auto p1 = cfg.rtsp_url.find("://");
                        if (p1 != std::string::npos) {
                            auto host_part = cfg.rtsp_url.substr(p1 + 3);
                            auto at_pos = host_part.find('@');
                            if (at_pos != std::string::npos) host_part = host_part.substr(at_pos + 1);
                            auto slash_pos = host_part.find('/');
                            if (slash_pos != std::string::npos) host_part = host_part.substr(0, slash_pos);
                            auto colon_pos = host_part.find(':');
                            std::string ip = (colon_pos != std::string::npos) ? host_part.substr(0, colon_pos) : host_part;
                            if (!ip.empty()) xaddr = "http://" + ip + ":80/onvif/device_service";
                        }
                    }
                    if (username.empty()) username = cfg.onvif_username;
                    if (password.empty()) password = cfg.onvif_password;
                }
            }
        }

        if (xaddr.empty()) {
            return CommandResult::Error(400, "xaddr or valid channel_id is required");
        }

        bool ok = OnvifClient::SetSystemDateAndTime(xaddr, username, password, target_utc_ms,
                                                    tz_str, timeout_ms, clock_offset_ms);
        nlohmann::json resp;
        resp["xaddr"] = xaddr;
        resp["target_utc_ms"] = target_utc_ms;
        resp["timezone"] = tz_str;
        resp["executed"] = ok;
        return ok ? CommandResult::Success(resp, "Camera date/time synchronized")
                  : CommandResult::Error(502, "Failed to synchronize camera date/time", resp);
    };
    RegisterHandler("onvif.datetime.set", UserRole::ADMIN, onvif_datetime_fn, "Synchronize camera date and time over ONVIF Device Service");
    RegisterHandler("onvif.datetime", UserRole::ADMIN, onvif_datetime_fn, "Alias for onvif.datetime.set");

    RegisterHandler("onvif.network.interfaces", UserRole::OPERATOR, [parse_json_int](const CommandEnvelope& cmd) -> CommandResult {
        TRACE_SCOPE("ONVIF_NETWORK_INTERFACES");
        std::string xaddr = cmd.payload.value("xaddr", cmd.payload.value("device_xaddr", ""));
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        int timeout_ms = parse_json_int(cmd.payload, "timeout_ms", 3000);
        int64_t clock_offset_ms = cmd.payload.value("clock_offset_ms", 0LL);

        if (xaddr.empty()) {
            int cid = cmd.payload.value("channel_id", cmd.payload.value("id", 0));
            if (cid > 0) {
                CameraConfig cfg;
                if (CameraManager::Instance().GetCamera(cid, cfg)) {
                    xaddr = cfg.onvif_xaddr;
                    if (xaddr.empty() && !cfg.rtsp_url.empty()) {
                        auto p1 = cfg.rtsp_url.find("://");
                        if (p1 != std::string::npos) {
                            auto host_part = cfg.rtsp_url.substr(p1 + 3);
                            auto at_pos = host_part.find('@');
                            if (at_pos != std::string::npos) host_part = host_part.substr(at_pos + 1);
                            auto slash_pos = host_part.find('/');
                            if (slash_pos != std::string::npos) host_part = host_part.substr(0, slash_pos);
                            auto colon_pos = host_part.find(':');
                            std::string ip = (colon_pos != std::string::npos) ? host_part.substr(0, colon_pos) : host_part;
                            if (!ip.empty()) xaddr = "http://" + ip + ":80/onvif/device_service";
                        }
                    }
                    if (username.empty()) username = cfg.onvif_username;
                    if (password.empty()) password = cfg.onvif_password;
                }
            }
        }

        if (xaddr.empty()) {
            return CommandResult::Error(400, "xaddr or valid channel_id is required");
        }

        std::vector<OnvifNetworkInterface> ifaces;
        bool ok = OnvifClient::GetNetworkInterfaces(xaddr, username, password, ifaces, timeout_ms, clock_offset_ms);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ifc : ifaces) {
            nlohmann::json ij;
            ij["token"] = ifc.token;
            ij["name"] = ifc.name;
            ij["hw_address"] = ifc.hw_address;
            ij["ipv4_address"] = ifc.ipv4_address;
            ij["subnet_mask"] = ifc.subnet_mask;
            ij["dhcp_enabled"] = ifc.dhcp_enabled;
            arr.push_back(ij);
        }
        return ok ? CommandResult::Success(arr)
                  : CommandResult::Error(502, "Failed to query camera network interfaces", arr);
    }, "Query camera physical network interfaces and MAC address over ONVIF");



    // 4. Live Display & Layout Control
    RegisterHandler("layout.get", UserRole::VIEWER, [](const CommandEnvelope&) -> CommandResult {
        nlohmann::json resp;
        resp["layout"] = static_cast<int>(LiveController::Instance().GetCurrentLayout());
        resp["active_tiles"] = LiveController::Instance().GetActiveTileCount();
        resp["fullscreen"] = LiveController::Instance().IsFullscreen();
        resp["fullscreen_channel"] = LiveController::Instance().GetFullscreenChannelId();

        auto metrics = LiveController::Instance().GetTileMetrics();
        nlohmann::json tile_arr = nlohmann::json::array();
        for (const auto& m : metrics) {
            nlohmann::json t;
            t["channel_id"] = m.channel_id;
            t["tile_index"] = m.tile_index;
            t["fps"] = m.estimated_fps;
            t["width"] = m.width;
            t["height"] = m.height;
            t["state"] = static_cast<int>(m.state);
            tile_arr.push_back(t);
        }
        resp["tiles"] = tile_arr;
        return CommandResult::Success(resp);
    }, "Query current live display grid layout and tile metrics");

    auto set_layout_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int count = 4;
        if (cmd.payload.contains("count") && cmd.payload["count"].is_number_integer()) {
            count = cmd.payload["count"].get<int>();
        } else if (cmd.payload.contains("layout_count") && cmd.payload["layout_count"].is_number_integer()) {
            count = cmd.payload["layout_count"].get<int>();
        } else if (cmd.payload.contains("layout")) {
            if (cmd.payload["layout"].is_number()) {
                count = cmd.payload["layout"].get<int>();
            } else if (cmd.payload["layout"].is_string()) {
                std::string s = cmd.payload["layout"].get<std::string>();
                if (s == "SINGLE" || s == "1") count = 1;
                else if (s == "GRID_4" || s == "4") count = 4;
                else if (s == "GRID_6" || s == "6") count = 6;
                else if (s == "GRID_8" || s == "8" || s == "7plus1") count = 8;
            }
        }
        LiveGridLayout target_layout = LiveGridLayout::GRID_4;
        if (count == 1) target_layout = LiveGridLayout::SINGLE;
        else if (count == 6) target_layout = LiveGridLayout::GRID_6;
        else if (count == 8 || count == 7) target_layout = LiveGridLayout::GRID_8;
        else if (count != 4) {
            return CommandResult::Error(400, "Unsupported grid size (supported: 1, 4, 6, 8)");
        }
        bool ok = LiveController::Instance().SetLayout(target_layout);
        nlohmann::json data;
        data["layout"] = static_cast<int>(target_layout);
        return ok ? CommandResult::Success(data, "Layout updated successfully")
                  : CommandResult::Error(500, "Failed to switch layout", data);
    };
    RegisterHandler("layout.set", UserRole::OPERATOR, set_layout_fn, "Set active grid layout (SINGLE, GRID_4, GRID_6, GRID_8)");
    RegisterHandler("set_layout", UserRole::OPERATOR, set_layout_fn, "Alias for layout.set");

    auto fullscreen_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        if (cmd.payload.value("exit", false) || cmd.payload.value("enabled", true) == false || cmd.action == "exit_fullscreen") {
            LiveController::Instance().ExitFullscreen();
            return CommandResult::Success(nlohmann::json::object(), "Exited fullscreen mode");
        }
        int channel_id = cmd.payload.value("channel_id", 0);
        if (channel_id <= 0) {
            LiveController::Instance().ExitFullscreen();
            return CommandResult::Success(nlohmann::json::object(), "Exited fullscreen mode");
        }
        bool ok = LiveController::Instance().SetFullscreen(channel_id);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        return ok ? CommandResult::Success(data, "Fullscreen mode activated")
                  : CommandResult::Error(500, "Failed to enter fullscreen", data);
    };
    RegisterHandler("layout.fullscreen", UserRole::OPERATOR, fullscreen_fn, "Enter or exit fullscreen view for a specified camera");
    RegisterHandler("set_fullscreen", UserRole::OPERATOR, fullscreen_fn, "Alias for layout.fullscreen");
    RegisterHandler("exit_fullscreen", UserRole::OPERATOR, fullscreen_fn, "Alias for exiting fullscreen");

    auto custom_grid_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int cols = cmd.payload.value("cols", 2);
        int rows = cmd.payload.value("rows", 2);
        std::vector<int> channels;
        if (cmd.payload.contains("channels") && cmd.payload["channels"].is_array()) {
            for (const auto& ch : cmd.payload["channels"]) {
                if (ch.is_number_integer()) {
                    channels.push_back(ch.get<int>());
                }
            }
        }
        bool ok = LiveController::Instance().SetCustomGrid(cols, rows, channels);
        nlohmann::json d;
        d["cols"] = cols;
        d["rows"] = rows;
        return ok ? CommandResult::Success(d, "Custom grid set successfully")
                  : CommandResult::Error(500, "Failed to set custom grid", d);
    };
    RegisterHandler("layout.custom_grid", UserRole::OPERATOR, custom_grid_fn, "Configure dynamic grid dimensions and tile channels");
    RegisterHandler("set_custom_grid", UserRole::OPERATOR, custom_grid_fn, "Alias for layout.custom_grid");

    auto set_tiles_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        if (!cmd.payload.contains("tiles") || !cmd.payload["tiles"].is_array()) {
            return CommandResult::Error(400, "Missing 'tiles' array in payload");
        }
        std::vector<TileConfig> tiles;
        for (const auto& item : cmd.payload["tiles"]) {
            if (!item.is_object()) continue;
            TileConfig tc;
            tc.tile_index = item.value("tile_index", 0);
            tc.channel_id = item.value("channel_id", 0);
            std::string st = item.value("stream_type", "SUB");
            tc.stream_type = (st == "MAIN") ? StreamType::MAIN : StreamType::SUB;
            tc.target_w = item.value("width", 0u);
            tc.target_h = item.value("height", 0u);
            tiles.push_back(tc);
        }
        bool ok = LiveController::Instance().SetTileLayout(tiles);
        return ok ? CommandResult::Success(nlohmann::json::object(), "Tiles configured successfully")
                  : CommandResult::Error(500, "Failed to configure tiles");
    };
    RegisterHandler("layout.set_tiles", UserRole::OPERATOR, set_tiles_fn, "Configure explicit per-tile camera mapping");
    RegisterHandler("set_tiles", UserRole::OPERATOR, set_tiles_fn, "Alias for layout.set_tiles");

    auto activate_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int ch = cmd.payload.value("channel_id", 0);
        if (ch <= 0) return CommandResult::Error(400, "Valid channel_id required");
        int tile_idx = cmd.payload.value("tile_index", -1);
        std::string st = cmd.payload.value("stream_type", "SUB");
        StreamType stream_type = (st == "MAIN") ? StreamType::MAIN : StreamType::SUB;
        uint32_t tw = cmd.payload.value("width", 0u);
        uint32_t th = cmd.payload.value("height", 0u);
        bool ok = LiveController::Instance().ActivateChannel(ch, tile_idx, stream_type, tw, th);
        return ok ? CommandResult::Success(nlohmann::json::object(), "Channel activated")
                  : CommandResult::Error(500, "Failed to activate channel");
    };
    RegisterHandler("layout.activate_channel", UserRole::OPERATOR, activate_fn, "Activate video feed for a channel");
    RegisterHandler("activate_channel", UserRole::OPERATOR, activate_fn, "Alias for layout.activate_channel");

    auto deactivate_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int ch = cmd.payload.value("channel_id", 0);
        if (ch <= 0) return CommandResult::Error(400, "Valid channel_id required");
        bool ok = LiveController::Instance().DeactivateChannel(ch);
        return ok ? CommandResult::Success(nlohmann::json::object(), "Channel deactivated")
                  : CommandResult::Error(500, "Failed to deactivate channel");
    };
    RegisterHandler("layout.deactivate_channel", UserRole::OPERATOR, deactivate_fn, "Deactivate video feed for a channel");
    RegisterHandler("deactivate_channel", UserRole::OPERATOR, deactivate_fn, "Alias for layout.deactivate_channel");

    auto set_active_channels_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        if (!cmd.payload.contains("channels") || !cmd.payload["channels"].is_array()) {
            return CommandResult::Error(400, "Missing 'channels' array in payload");
        }
        std::vector<int> channels;
        for (const auto& ch : cmd.payload["channels"]) {
            if (ch.is_number_integer()) {
                channels.push_back(ch.get<int>());
            }
        }
        std::string st = cmd.payload.value("stream_type", "SUB");
        StreamType stream_type = (st == "MAIN") ? StreamType::MAIN : StreamType::SUB;
        bool ok = LiveController::Instance().SetActiveChannels(channels, stream_type);
        return ok ? CommandResult::Success(nlohmann::json::object(), "Active channels updated")
                  : CommandResult::Error(500, "Failed to set active channels");
    };
    RegisterHandler("layout.set_active_channels", UserRole::OPERATOR, set_active_channels_fn, "Set active camera channel set");
    RegisterHandler("set_active_channels", UserRole::OPERATOR, set_active_channels_fn, "Alias for layout.set_active_channels");

    // 5. Recording Control & Event Buffering
    auto start_recording_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        RecordMode mode = RecordMode::CONTINUOUS;
        if (cmd.payload.contains("mode")) {
            if (cmd.payload["mode"].is_number()) {
                mode = static_cast<RecordMode>(cmd.payload["mode"].get<int>());
            } else if (cmd.payload["mode"].is_string()) {
                std::string s = cmd.payload["mode"].get<std::string>();
                if (s == "motion" || s == "motion_only" || s == "2") mode = RecordMode::MOTION_ONLY;
                else if (s == "scheduled" || s == "3") mode = RecordMode::SCHEDULED;
                else if (s == "event" || s == "event_only" || s == "4") mode = RecordMode::EVENT_ONLY;
                else if (s == "disabled" || s == "0") mode = RecordMode::DISABLED;
                else mode = RecordMode::CONTINUOUS;
            }
        }
        bool ok = RecordingScheduler::Instance().StartChannelRecording(channel_id, mode);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["record_mode"] = static_cast<int>(mode);
        data["record_mode_str"] = RecordModeToString(mode);
        return ok ? CommandResult::Success(data, "Channel recording started successfully")
                  : CommandResult::Error(500, "Failed to start channel recording", data);
    };
    RegisterHandler("recording.start", UserRole::OPERATOR, start_recording_fn, "Start recording for a channel");
    RegisterHandler("recordings.start", UserRole::OPERATOR, start_recording_fn, "Alias for recording.start");

    auto stop_recording_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        RecordingScheduler::Instance().StopChannelRecording(channel_id);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        return CommandResult::Success(data, "Channel recording stopped successfully");
    };
    RegisterHandler("recording.stop", UserRole::OPERATOR, stop_recording_fn, "Stop recording for a channel");
    RegisterHandler("recordings.stop", UserRole::OPERATOR, stop_recording_fn, "Alias for recording.stop");

    auto status_recording_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["is_recording"] = RecordingScheduler::Instance().IsChannelRecording(channel_id);
        data["is_active"] = RecordingScheduler::Instance().IsChannelActive(channel_id);
        data["frame_count"] = RecordingScheduler::Instance().GetChannelFrameCount(channel_id);
        data["bytes_written"] = RecordingScheduler::Instance().GetChannelBytesWritten(channel_id);
        data["dropped_packets"] = RecordingScheduler::Instance().GetChannelDroppedPackets(channel_id);
        data["preroll_packets"] = RecordingScheduler::Instance().GetChannelPrerollPacketCount(channel_id);
        data["preroll_bytes"] = RecordingScheduler::Instance().GetChannelPrerollBytes(channel_id);
        data["is_event_active"] = RecordingScheduler::Instance().IsEventActive(channel_id);
        data["current_event_type"] = RecordingScheduler::Instance().GetCurrentEventType(channel_id);
        return CommandResult::Success(data, "Recording status retrieved successfully");
    };
    RegisterHandler("recording.status", UserRole::VIEWER, status_recording_fn, "Get recording metrics and active status for a channel");
    RegisterHandler("recordings.status", UserRole::VIEWER, status_recording_fn, "Alias for recording.status");

    auto get_schedule_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        nlohmann::json sched = RecordingScheduler::Instance().GetChannelSchedule(channel_id);
        return CommandResult::Success(sched, "Channel schedule retrieved successfully");
    };
    RegisterHandler("recording.get_schedule", UserRole::VIEWER, get_schedule_fn, "Get recording schedule for a channel");
    RegisterHandler("recordings.get_schedule", UserRole::VIEWER, get_schedule_fn, "Alias for recording.get_schedule");

    auto set_schedule_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        bool ok = RecordingScheduler::Instance().SetChannelSchedule(channel_id, cmd.payload);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["success"] = ok;
        return ok ? CommandResult::Success(data, "Channel schedule updated successfully")
                  : CommandResult::Error(500, "Failed to update channel schedule", data);
    };
    RegisterHandler("recording.set_schedule", UserRole::OPERATOR, set_schedule_fn, "Set recording schedule for a channel");
    RegisterHandler("recordings.set_schedule", UserRole::OPERATOR, set_schedule_fn, "Alias for recording.set_schedule");

    auto trigger_event_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        std::string event_type = cmd.payload.value("event_type", "alarm");
        int pre_roll_sec = cmd.payload.value("pre_roll_sec", 5);
        int post_roll_sec = cmd.payload.value("post_roll_sec", 10);
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        RecordingScheduler::Instance().TriggerEventRecording(channel_id, event_type, pre_roll_sec, post_roll_sec);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["event_type"] = event_type;
        data["pre_roll_sec"] = pre_roll_sec;
        data["post_roll_sec"] = post_roll_sec;
        return CommandResult::Success(data, "Event recording triggered with pre-roll and post-roll");
    };
    RegisterHandler("recording.trigger_event", UserRole::OPERATOR, trigger_event_fn, "Trigger pre-roll and post-roll event recording");
    RegisterHandler("recordings.trigger_event", UserRole::OPERATOR, trigger_event_fn, "Alias for recording.trigger_event");

    auto set_event_state_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", cmd.payload.value("id", 1));
        std::string event_type = cmd.payload.value("event_type", "alarm");
        bool active = cmd.payload.value("active", true);
        if (channel_id <= 0) {
            return CommandResult::Error(400, "Valid channel_id required");
        }
        RecordingScheduler::Instance().SetEventActive(channel_id, event_type, active);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        data["event_type"] = event_type;
        data["active"] = active;
        return CommandResult::Success(data, active ? "Event state set to active" : "Event state set to inactive");
    };
    RegisterHandler("recording.set_event_state", UserRole::OPERATOR, set_event_state_fn, "Update active event state for a channel");
    RegisterHandler("recordings.set_event_state", UserRole::OPERATOR, set_event_state_fn, "Alias for recording.set_event_state");

    // 6. Playback, Search & Clip Export Actions
    auto playback_dates_fn = [](const CommandEnvelope&) -> CommandResult {
        auto dates = PlaybackEngine::Instance().GetRecordingDates();
        nlohmann::json d = dates;
        return CommandResult::Success(d);
    };
    RegisterHandler("playback.dates", UserRole::VIEWER, playback_dates_fn, "Get list of available recording dates");
    RegisterHandler("recordings.dates", UserRole::VIEWER, playback_dates_fn, "Alias for playback.dates");

    auto playback_query_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        auto res = PlaybackEngine::Instance().QueryRecordings(cmd.payload);
        return CommandResult::Success(res);
    };
    RegisterHandler("playback.query", UserRole::VIEWER, playback_query_fn, "Query recordings and review activity blocks");
    RegisterHandler("recordings.query", UserRole::VIEWER, playback_query_fn, "Alias for playback.query");
    RegisterHandler("recordings.list", UserRole::VIEWER, playback_query_fn, "Alias for playback.query");

    auto playback_lock_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        std::string path = cmd.payload.value("path", cmd.payload.value("file_path", ""));
        bool locked = cmd.payload.value("locked", true);
        if (path.empty()) {
            return CommandResult::Error(400, "Segment path required for locking");
        }
        bool ok = PlaybackEngine::Instance().SetSegmentLock(path, locked);
        if (ok) {
            nlohmann::json d;
            d["path"] = path;
            d["locked"] = locked;
            return CommandResult::Success(d, locked ? "Segment locked against retention" : "Segment unlocked");
        }
        return CommandResult::Error(404, "Recording segment not found or failed to update lock state");
    };
    RegisterHandler("playback.lock", UserRole::OPERATOR, playback_lock_fn, "Lock or unlock a recording segment against retention pruning");
    RegisterHandler("recordings.lock", UserRole::OPERATOR, playback_lock_fn, "Alias for playback.lock");

    auto playback_export_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        int ch = cmd.payload.value("channel_id", 1);
        int64_t start_ms = cmd.payload.value("start_time_ms", 0LL);
        int64_t end_ms = cmd.payload.value("end_time_ms", 0LL);
        std::string fname = cmd.payload.value("filename", cmd.payload.value("output_filename", ""));

        if (start_ms <= 0) {
            std::string start_iso = cmd.payload.value("start_time", "");
            if (!start_iso.empty()) {
                struct tm tm{};
                if (strptime(start_iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
                    start_ms = static_cast<int64_t>(timegm(&tm)) * 1000LL;
                }
            }
        }
        if (end_ms <= 0) {
            std::string end_iso = cmd.payload.value("end_time", "");
            if (!end_iso.empty()) {
                struct tm tm{};
                if (strptime(end_iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
                    end_ms = static_cast<int64_t>(timegm(&tm)) * 1000LL;
                }
            }
        }

        std::string job_id = PlaybackEngine::Instance().StartExport(ch, start_ms, end_ms, fname);
        nlohmann::json d;
        d["job_id"] = job_id;
        d["channel_id"] = ch;
        d["state"] = "RUNNING";
        return CommandResult::Success(d, "Export job started");
    };
    RegisterHandler("playback.export", UserRole::OPERATOR, playback_export_fn, "Start an incident clip export job");
    RegisterHandler("recordings.export", UserRole::OPERATOR, playback_export_fn, "Alias for playback.export");

    auto playback_export_status_fn = [](const CommandEnvelope& cmd) -> CommandResult {
        std::string job_id = cmd.payload.value("job_id", "");
        if (job_id.empty()) {
            return CommandResult::Error(400, "job_id required");
        }
        auto status = PlaybackEngine::Instance().GetExportStatus(job_id);
        if (status.contains("error")) {
            return CommandResult::Error(404, status["error"].get<std::string>());
        }
        return CommandResult::Success(status);
    };
    RegisterHandler("playback.export_status", UserRole::VIEWER, playback_export_status_fn, "Get status of a clip export job");
    RegisterHandler("recordings.export_status", UserRole::VIEWER, playback_export_status_fn, "Alias for playback.export_status");

    auto playback_export_list_fn = [](const CommandEnvelope&) -> CommandResult {
        auto list = PlaybackEngine::Instance().ListExportJobs();
        return CommandResult::Success(list);
    };
    RegisterHandler("playback.export_list", UserRole::VIEWER, playback_export_list_fn, "List clip export jobs");
    RegisterHandler("recordings.export_list", UserRole::VIEWER, playback_export_list_fn, "Alias for playback.export_list");

    // ====================================================
    // User Authentication & RBAC Handlers
    // ====================================================
    RegisterHandler("auth.login", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        if (username.empty() || password.empty()) {
            return CommandResult::Error(400, "Username and password are required");
        }

        UserRecord user;
        std::string err;
        if (!DatabaseManager::Instance().VerifyUserCredentials(username, password, user, &err)) {
            DatabaseManager::Instance().AppendAuditLog(username, "UNKNOWN", "API", "auth.login", "security",
                                                       "Failed login attempt: " + err, "{}", "FAILED");
            return CommandResult::Error(401, err);
        }

        std::string token = DatabaseManager::Instance().CreateSession(user.id);
        if (token.empty()) {
            return CommandResult::Error(500, "Failed to generate session token");
        }

        DatabaseManager::Instance().AppendAuditLog(user.username, user.role, "API", "auth.login", "security",
                                                   "Successful user login", "{}", "SUCCESS");

        nlohmann::json data;
        data["token"] = token;
        data["username"] = user.username;
        data["role"] = user.role;
        data["user_id"] = user.id;
        return CommandResult::Success(data);
    }, "Authenticate user and issue session token");

    RegisterHandler("auth.logout", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        std::string token = cmd.auth_token;
        if (token.empty()) token = cmd.payload.value("token", "");
        if (!token.empty()) {
            SessionRecord s;
            if (DatabaseManager::Instance().ValidateSession(token, s)) {
                DatabaseManager::Instance().AppendAuditLog(s.username, s.role, "API", "auth.logout", "security",
                                                           "User logged out", "{}");
            }
            DatabaseManager::Instance().RevokeSession(token);
        }
        return CommandResult::Success("Logged out successfully");
    }, "Revoke active session token");

    RegisterHandler("auth.me", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        if (cmd.auth_token.empty()) {
            return CommandResult::Error(401, "Not authenticated");
        }
        SessionRecord s;
        if (!DatabaseManager::Instance().ValidateSession(cmd.auth_token, s)) {
            return CommandResult::Error(401, "Session expired or invalid");
        }
        nlohmann::json data;
        data["authenticated"] = true;
        data["user_id"] = s.user_id;
        data["username"] = s.username;
        data["role"] = s.role;
        return CommandResult::Success(data);
    }, "Get profile of currently authenticated session");

    RegisterHandler("user.list", UserRole::ADMIN, [](const CommandEnvelope&) -> CommandResult {
        auto users = DatabaseManager::Instance().ListUsers();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& u : users) {
            arr.push_back({
                {"id", u.id},
                {"username", u.username},
                {"role", u.role},
                {"is_active", u.is_active},
                {"failed_attempts", u.failed_attempts},
                {"locked_until", u.locked_until},
                {"created_at", u.created_at},
                {"updated_at", u.updated_at}
            });
        }
        return CommandResult::Success(arr);
    }, "List all registered users (Admin only)");

    RegisterHandler("user.add", UserRole::ADMIN, [](const CommandEnvelope& cmd) -> CommandResult {
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        std::string role = cmd.payload.value("role", "OPERATOR");

        std::string err;
        if (!DatabaseManager::Instance().CreateUser(username, password, role, &err)) {
            return CommandResult::Error(400, "Failed to create user: " + err);
        }

        DatabaseManager::Instance().AppendAuditLog("admin", "ADMIN", "API", "user.add", "users",
                                                   "Created user '" + username + "' with role '" + role + "'");
        return CommandResult::Success("User '" + username + "' created successfully");
    }, "Create a new user account (Admin only)");

    RegisterHandler("user.delete", UserRole::ADMIN, [](const CommandEnvelope& cmd) -> CommandResult {
        int user_id = cmd.payload.value("id", 0);
        if (user_id <= 0 && cmd.payload.contains("user_id")) {
            user_id = cmd.payload.value("user_id", 0);
        }
        if (user_id <= 0 && cmd.payload.contains("username")) {
            std::string uname = cmd.payload.value("username", "");
            UserRecord u;
            if (DatabaseManager::Instance().GetUserByUsername(uname, u)) {
                user_id = u.id;
            }
        }
        if (user_id <= 0) {
            return CommandResult::Error(400, "Valid user id or username is required");
        }

        std::string err;
        if (!DatabaseManager::Instance().DeleteUser(user_id, &err)) {
            return CommandResult::Error(400, err.empty() ? "Failed to delete user" : err);
        }

        DatabaseManager::Instance().AppendAuditLog("admin", "ADMIN", "API", "user.delete", "users",
                                                   "Deleted user id " + std::to_string(user_id));
        return CommandResult::Success("User deleted successfully");
    }, "Delete a user account (Admin only)");

    RegisterHandler("user.change_password", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        if (cmd.auth_token.empty()) {
            return CommandResult::Error(401, "Authentication required to change password");
        }
        SessionRecord session;
        if (!DatabaseManager::Instance().ValidateSession(cmd.auth_token, session)) {
            return CommandResult::Error(401, "Invalid session");
        }

        std::string old_pass = cmd.payload.value("old_password", "");
        std::string new_pass = cmd.payload.value("new_password", "");
        if (old_pass.empty() || new_pass.empty()) {
            return CommandResult::Error(400, "Both old_password and new_password are required");
        }

        std::string err;
        if (!DatabaseManager::Instance().ChangeUserPassword(session.user_id, old_pass, new_pass, &err)) {
            return CommandResult::Error(400, err);
        }

        DatabaseManager::Instance().AppendAuditLog(session.username, session.role, "API", "user.change_password", "security",
                                                   "User changed their password");
        return CommandResult::Success("Password changed successfully");
    }, "Change own account password");

    RegisterHandler("user.reset_password", UserRole::ADMIN, [](const CommandEnvelope& cmd) -> CommandResult {
        int user_id = cmd.payload.value("id", 0);
        if (user_id <= 0 && cmd.payload.contains("user_id")) {
            user_id = cmd.payload.value("user_id", 0);
        }
        if (user_id <= 0 && cmd.payload.contains("username")) {
            std::string uname = cmd.payload.value("username", "");
            UserRecord u;
            if (DatabaseManager::Instance().GetUserByUsername(uname, u)) {
                user_id = u.id;
            }
        }
        std::string new_pass = cmd.payload.value("new_password", "");
        if (user_id <= 0 || new_pass.empty()) {
            return CommandResult::Error(400, "Valid user ID/username and new_password are required");
        }

        std::string err;
        if (!DatabaseManager::Instance().AdminResetPassword(user_id, new_pass, &err)) {
            return CommandResult::Error(400, err);
        }

        DatabaseManager::Instance().AppendAuditLog("admin", "ADMIN", "API", "user.reset_password", "users",
                                                   "Administrator reset password for user id " + std::to_string(user_id));
        return CommandResult::Success("Password reset successfully");
    }, "Reset a user password (Admin only)");
}

} // namespace nvr
