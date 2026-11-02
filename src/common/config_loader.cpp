#include "nvr/common/config_loader.h"
#include "nvr/common/logger.h"
#include "3rdparty/nlohmann/json.hpp"
#include <fstream>

using json = nlohmann::json;

namespace nvr {

NvrConfig LoadNvrConfig(const std::string& config_path) {
    NvrConfig config;
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
            if (sys.contains("log_dir")) config.system.log_dir = sys["log_dir"];
            if (sys.contains("max_channels")) config.system.max_channels = sys["max_channels"];
        }

        if (j.contains("storage")) {
            auto& st = j["storage"];
            if (st.contains("recording_path")) config.storage.recording_path = st["recording_path"];
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

        if (j.contains("ai")) {
            auto& ai = j["ai"];
            if (ai.contains("enabled")) config.ai.enabled = ai["enabled"];
            if (ai.contains("max_concurrent_models")) config.ai.max_concurrent_models = ai["max_concurrent_models"];
            if (ai.contains("motion_threshold")) config.ai.motion_threshold = ai["motion_threshold"];
        }

        if (j.contains("api")) {
            auto& a = j["api"];
            if (a.contains("enabled")) config.api.enabled = a["enabled"];
            if (a.contains("port")) config.api.port = a["port"];
            if (a.contains("web_root")) config.api.web_root = a["web_root"];
            if (a.contains("bind_address")) config.api.bind_address = a["bind_address"];
            if (a.contains("enforce_https")) config.api.enforce_https = a["enforce_https"];
            if (a.contains("enable_hsts")) config.api.enable_hsts = a["enable_hsts"];
            if (a.contains("cors_allowed_origins")) {
                config.api.cors_allowed_origins.clear();
                if (a["cors_allowed_origins"].is_array()) {
                    for (const auto& item : a["cors_allowed_origins"]) {
                        if (item.is_string()) config.api.cors_allowed_origins.push_back(item.get<std::string>());
                    }
                } else if (a["cors_allowed_origins"].is_string()) {
                    config.api.cors_allowed_origins.push_back(a["cors_allowed_origins"].get<std::string>());
                }
            }
        }

        if (j.contains("cameras") && j["cameras"].is_array()) {
            config.cameras.clear();
            for (const auto& item : j["cameras"]) {
                CameraConfig cam;
                cam.id = item.value("id", 1);
                cam.name = item.value("name", "Camera");
                cam.rtsp_url = item.value("rtsp_url", "");
                cam.sub_rtsp_url = item.value("sub_rtsp_url", "");
                cam.onvif_xaddr = item.value("onvif_xaddr", "");
                cam.onvif_username = item.value("onvif_username", "admin");
                cam.onvif_password = item.value("onvif_password", "");
                cam.enabled = item.value("enabled", true);
                cam.record_mode = static_cast<RecordMode>(item.value("record_mode", 1));
                cam.reconnect_delay_sec = item.value("reconnect_delay_sec", 3);
                config.cameras.push_back(cam);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to parse configuration file " << config_path << ": " << e.what();
    }

    return config;
}

} // namespace nvr
