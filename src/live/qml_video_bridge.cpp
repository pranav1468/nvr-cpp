#include "nvr/live/qml_video_bridge.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "3rdparty/nlohmann/json.hpp"

using json = nlohmann::json;

namespace nvr {

QmlVideoBridge& QmlVideoBridge::Instance() {
    static QmlVideoBridge instance;
    return instance;
}

bool QmlVideoBridge::Initialize(int window_width, int window_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    width_ = window_width;
    height_ = window_height;
    tiles_.clear();
    return true;
}

void QmlVideoBridge::RenderTile(int tile_index, int channel_id, const DecodedFramePtr& frame) {
    FrameRenderCallback cb;
    uint64_t rev = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& t = tiles_[tile_index];
        t.channel_id = channel_id;
        t.revision++;
        t.frame = frame;
        rev = t.revision;
        cb = render_callback_;
    }

    if (cb) {
        cb(tile_index, channel_id, rev);
    }
}

void QmlVideoBridge::ClearTile(int tile_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    tiles_.erase(tile_index);
}

void QmlVideoBridge::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    tiles_.clear();
}

const char* QmlVideoBridge::GetBackendName() const {
    return "QmlVideoBridge";
}

DecodedFramePtr QmlVideoBridge::GetTileFrame(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.frame : nullptr;
}

uint64_t QmlVideoBridge::GetTileRevision(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.revision : 0;
}

int QmlVideoBridge::GetTileChannelId(int tile_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tiles_.find(tile_index);
    return (it != tiles_.end()) ? it->second.channel_id : -1;
}

void QmlVideoBridge::RequestLayout(int layout_count) {
    if (layout_count == 1) {
        LiveController::Instance().SetLayout(LiveGridLayout::SINGLE);
    } else if (layout_count == 6) {
        LiveController::Instance().SetLayout(LiveGridLayout::GRID_6);
    } else if (layout_count == 8) {
        LiveController::Instance().SetLayout(LiveGridLayout::GRID_8);
    } else {
        LiveController::Instance().SetLayout(LiveGridLayout::GRID_4);
    }
}

void QmlVideoBridge::RequestFullscreen(int channel_id) {
    LiveController::Instance().SetFullscreen(channel_id);
}

bool QmlVideoBridge::ExecuteCommandJson(const std::string& command_json) {
    try {
        auto j = json::parse(command_json);
        if (!j.is_object() || !j.contains("action") || !j["action"].is_string()) {
            return false;
        }

        std::string action = j["action"].get<std::string>();

        if (action == "set_layout") {
            int count = 4;
            if (j.contains("count") && j["count"].is_number_integer()) {
                count = j["count"].get<int>();
            } else if (j.contains("layout") && j["layout"].is_string()) {
                std::string lstr = j["layout"].get<std::string>();
                if (lstr == "SINGLE" || lstr == "1") count = 1;
                else if (lstr == "GRID_4" || lstr == "4") count = 4;
                else if (lstr == "GRID_6" || lstr == "6") count = 6;
                else if (lstr == "GRID_8" || lstr == "8") count = 8;
            }
            RequestLayout(count);
            return true;
        }

        if (action == "set_custom_grid") {
            int cols = j.value("cols", 2);
            int rows = j.value("rows", 2);
            std::vector<int> channels;
            if (j.contains("channels") && j["channels"].is_array()) {
                for (const auto& ch : j["channels"]) {
                    if (ch.is_number_integer()) {
                        channels.push_back(ch.get<int>());
                    }
                }
            }
            return LiveController::Instance().SetCustomGrid(cols, rows, channels);
        }

        if (action == "set_tiles") {
            if (!j.contains("tiles") || !j["tiles"].is_array()) {
                return false;
            }
            std::vector<TileConfig> tiles;
            for (const auto& item : j["tiles"]) {
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
            return LiveController::Instance().SetTileLayout(tiles);
        }

        if (action == "set_fullscreen") {
            if (!j.contains("channel_id") || !j["channel_id"].is_number_integer()) {
                return false;
            }
            int ch = j["channel_id"].get<int>();
            return LiveController::Instance().SetFullscreen(ch);
        }

        if (action == "exit_fullscreen") {
            return LiveController::Instance().ExitFullscreen();
        }

        if (action == "activate_channel") {
            if (!j.contains("channel_id") || !j["channel_id"].is_number_integer()) {
                return false;
            }
            int ch = j["channel_id"].get<int>();
            int tile_idx = j.value("tile_index", -1);
            std::string st = j.value("stream_type", "SUB");
            StreamType stream_type = (st == "MAIN") ? StreamType::MAIN : StreamType::SUB;
            uint32_t tw = j.value("width", 0u);
            uint32_t th = j.value("height", 0u);
            return LiveController::Instance().ActivateChannel(ch, tile_idx, stream_type, tw, th);
        }

        if (action == "deactivate_channel") {
            if (!j.contains("channel_id") || !j["channel_id"].is_number_integer()) {
                return false;
            }
            int ch = j["channel_id"].get<int>();
            return LiveController::Instance().DeactivateChannel(ch);
        }

        if (action == "set_active_channels") {
            if (!j.contains("channels") || !j["channels"].is_array()) {
                return false;
            }
            std::vector<int> channels;
            for (const auto& ch : j["channels"]) {
                if (ch.is_number_integer()) {
                    channels.push_back(ch.get<int>());
                }
            }
            std::string st = j.value("stream_type", "SUB");
            StreamType stream_type = (st == "MAIN") ? StreamType::MAIN : StreamType::SUB;
            return LiveController::Instance().SetActiveChannels(channels, stream_type);
        }

        return false;
    } catch (const std::exception& e) {
        return false;
    }
}

std::string QmlVideoBridge::GetGridStateJson() const {
    json j;
    bool is_fs = LiveController::Instance().IsFullscreen();
    j["is_fullscreen"] = is_fs;
    j["fullscreen_channel_id"] = LiveController::Instance().GetFullscreenChannelId();
    j["layout_count"] = LiveController::Instance().GetActiveTileCount();
    j["width"] = width_;
    j["height"] = height_;

    auto metrics = LiveController::Instance().GetTileMetrics();
    json tiles_arr = json::array();

    for (const auto& m : metrics) {
        json tile_obj;
        tile_obj["tile_index"] = m.tile_index;
        tile_obj["channel_id"] = m.channel_id;
        tile_obj["stream_type"] = (m.stream_type == StreamType::MAIN) ? "MAIN" : "SUB";
        tile_obj["width"] = m.width;
        tile_obj["height"] = m.height;
        tile_obj["enqueued"] = m.enqueued_frames;
        tile_obj["dropped"] = m.dropped_frames;
        tile_obj["rendered"] = m.rendered_frames;
        tile_obj["fps"] = m.estimated_fps;
        tile_obj["is_recording"] = RecordingScheduler::Instance().IsChannelRecording(m.channel_id);
        tile_obj["is_online"] = CameraManager::Instance().IsCameraOnline(m.channel_id);
        tiles_arr.push_back(tile_obj);
    }

    j["tiles"] = tiles_arr;
    return j.dump();
}

void QmlVideoBridge::SetFrameRenderCallback(FrameRenderCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    render_callback_ = std::move(cb);
}

} // namespace nvr
