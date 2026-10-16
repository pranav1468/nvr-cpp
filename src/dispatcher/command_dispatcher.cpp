#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/dispatcher/rbac_validator.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/live/live_controller.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/storage/segment_index.h"
#include "nvr/storage/retention_manager.h"

#include <sys/sysinfo.h>
#include <cstdlib>

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

    // RBAC validation
    if (reg.min_role != UserRole::NONE) {
        UserRole caller_role = UserRole::NONE;
        bool valid = RbacValidator::Instance().ValidateToken(cmd.auth_token, reg.min_role, &caller_role);
        if (!valid) {
            if (caller_role == UserRole::NONE) {
                LOG_WARN << "CommandDispatcher: Unauthorized request for action '" << cmd.action 
                         << "' (token invalid, missing, or expired)";
                return CommandResult::Error(401, "Unauthorized: Invalid, expired, or missing auth token");
            } else {
                LOG_WARN << "CommandDispatcher: Forbidden request for action '" << cmd.action 
                         << "' (caller role " << UserRoleToString(caller_role) 
                         << " < required " << UserRoleToString(reg.min_role) << ")";
                return CommandResult::Error(403, "Forbidden: Insufficient privileges for action");
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

void CommandDispatcher::RegisterDefaultHandlers() {
    // 1. Authentication
    RegisterHandler("auth.login", UserRole::NONE, [](const CommandEnvelope& cmd) -> CommandResult {
        std::string username = cmd.payload.value("username", "");
        std::string password = cmd.payload.value("password", "");
        if (username.empty() || password.empty()) {
            return CommandResult::Error(400, "Username and password are required");
        }
        std::string token;
        UserRole role = UserRole::NONE;
        if (!RbacValidator::Instance().Authenticate(username, password, token, role)) {
            return CommandResult::Error(401, "Invalid username or password");
        }
        nlohmann::json resp;
        resp["token"] = token;
        resp["username"] = username;
        resp["role"] = UserRoleToString(role);
        return CommandResult::Success(resp, "Authenticated successfully");
    }, "Authenticate credentials and obtain session token");

    RegisterHandler("auth.logout", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        RbacValidator::Instance().RevokeSession(cmd.auth_token);
        return CommandResult::Success(nlohmann::json::object(), "Session revoked successfully");
    }, "Revoke active session token");

    // 2. System Status & Telemetry
    RegisterHandler("system.status", UserRole::VIEWER, [](const CommandEnvelope&) -> CommandResult {
        nlohmann::json resp;
        resp["device_name"] = "NVR_iMX95";
        resp["version"] = "2.0.0";
        resp["status"] = "ONLINE";
        resp["free_storage_bytes"] = RetentionManager::Instance().GetFreeDiskSpaceBytes("./recordings");
        resp["total_storage_bytes"] = RetentionManager::Instance().GetTotalDiskSpaceBytes("./recordings");
        resp["total_segments"] = SegmentIndex::Instance().GetTotalSegmentCount();
        resp["total_recording_size_bytes"] = SegmentIndex::Instance().GetTotalRecordingSizeBytes();

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

        return CommandResult::Success(resp);
    }, "Query system hardware and storage telemetry");

    // 3. Camera Management
    RegisterHandler("camera.list", UserRole::VIEWER, [](const CommandEnvelope&) -> CommandResult {
        auto cameras = CameraManager::Instance().GetCameras();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cameras) {
            nlohmann::json item;
            item["id"] = c.id;
            item["name"] = c.name;
            item["rtsp_url"] = c.rtsp_url;
            item["sub_rtsp_url"] = c.sub_rtsp_url;
            item["enabled"] = c.enabled;
            item["online"] = CameraManager::Instance().IsCameraOnline(c.id);
            item["recording"] = RecordingScheduler::Instance().IsChannelRecording(c.id);
            item["frames_recorded"] = RecordingScheduler::Instance().GetChannelFrameCount(c.id);
            item["bytes_recorded"] = RecordingScheduler::Instance().GetChannelBytesWritten(c.id);
            arr.push_back(item);
        }
        return CommandResult::Success(arr);
    }, "List all configured cameras and their runtime statuses");

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

    RegisterHandler("layout.set", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        if (!cmd.payload.contains("layout")) {
            return CommandResult::Error(400, "Missing 'layout' field (expected integer grid layout or string)");
        }
        LiveGridLayout target_layout = LiveGridLayout::GRID_4;
        if (cmd.payload["layout"].is_number()) {
            int val = cmd.payload["layout"].get<int>();
            if (val == 1) target_layout = LiveGridLayout::SINGLE;
            else if (val == 4) target_layout = LiveGridLayout::GRID_4;
            else if (val == 6) target_layout = LiveGridLayout::GRID_6;
            else if (val == 8) target_layout = LiveGridLayout::GRID_8;
            else {
                return CommandResult::Error(400, "Unsupported grid size (supported: 1, 4, 6, 8)");
            }
        } else if (cmd.payload["layout"].is_string()) {
            std::string s = cmd.payload["layout"].get<std::string>();
            if (s == "SINGLE" || s == "1") target_layout = LiveGridLayout::SINGLE;
            else if (s == "GRID_4" || s == "4") target_layout = LiveGridLayout::GRID_4;
            else if (s == "GRID_6" || s == "6") target_layout = LiveGridLayout::GRID_6;
            else if (s == "GRID_8" || s == "8") target_layout = LiveGridLayout::GRID_8;
            else {
                return CommandResult::Error(400, "Unknown layout name: " + s);
            }
        }
        bool ok = LiveController::Instance().SetLayout(target_layout);
        nlohmann::json data;
        data["layout"] = static_cast<int>(target_layout);
        return ok ? CommandResult::Success(data, "Layout updated successfully")
                  : CommandResult::Error(500, "Failed to switch layout", data);
    }, "Set active grid layout (SINGLE, GRID_4, GRID_6, GRID_8)");

    RegisterHandler("layout.fullscreen", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        if (cmd.payload.value("exit", false)) {
            LiveController::Instance().ExitFullscreen();
            return CommandResult::Success(nlohmann::json::object(), "Exited fullscreen mode");
        }
        int channel_id = cmd.payload.value("channel_id", 0);
        if (channel_id <= 0) {
            return CommandResult::Error(400, "channel_id is required for fullscreen mode");
        }
        bool ok = LiveController::Instance().SetFullscreen(channel_id);
        nlohmann::json data;
        data["channel_id"] = channel_id;
        return ok ? CommandResult::Success(data, "Fullscreen mode activated")
                  : CommandResult::Error(500, "Failed to enter fullscreen", data);
    }, "Enter or exit fullscreen view for a specified camera");

    // 5. Recordings & Storage
    RegisterHandler("recordings.query", UserRole::VIEWER, [](const CommandEnvelope& cmd) -> CommandResult {
        int channel_id = cmd.payload.value("channel_id", 1);
        int64_t start_ms = cmd.payload.value("start_ms", 0LL);
        int64_t end_ms = cmd.payload.value("end_ms", 9999999999999LL);

        auto segments = SegmentIndex::Instance().QuerySegments(channel_id, start_ms, end_ms);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& seg : segments) {
            nlohmann::json item;
            item["id"] = seg.id;
            item["channel_id"] = seg.channel_id;
            item["file_path"] = seg.file_path;
            item["start_time_ms"] = seg.start_time_ms;
            item["end_time_ms"] = seg.end_time_ms;
            item["duration_ms"] = seg.duration_ms;
            item["file_size_bytes"] = seg.file_size_bytes;
            item["frame_count"] = seg.frame_count;
            item["codec"] = CodecToString(seg.codec);
            item["is_locked"] = seg.is_locked;
            arr.push_back(item);
        }
        return CommandResult::Success(arr);
    }, "Query recorded video segments by channel and time interval");

    RegisterHandler("recordings.lock", UserRole::OPERATOR, [](const CommandEnvelope& cmd) -> CommandResult {
        int64_t segment_id = cmd.payload.value("segment_id", 0LL);
        bool locked = cmd.payload.value("locked", true);
        if (segment_id <= 0) {
            return CommandResult::Error(400, "Valid segment_id is required");
        }
        bool ok = SegmentIndex::Instance().LockSegment(segment_id, locked);
        nlohmann::json data;
        data["segment_id"] = segment_id;
        data["locked"] = locked;
        return ok ? CommandResult::Success(data, locked ? "Segment locked against retention pruning" : "Segment unlocked")
                  : CommandResult::Error(404, "Segment not found or could not update lock", data);
    }, "Lock or unlock a recorded segment to prevent retention pruning");
}

} // namespace nvr
