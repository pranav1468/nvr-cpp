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

const char* GetBackendName() {
    return "QmlVideoBridge";
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

void QmlVideoBridge::RequestExitFullscreen() {
    LiveController::Instance().ExitFullscreen();
}

std::string QmlVideoBridge::GetGridStateJson() const {
    json j;
    bool is_fs = LiveController::Instance().IsFullscreen();
    j["is_fullscreen"] = is_fs;
    j["fullscreen_channel_id"] = LiveController::Instance().GetFullscreenChannelId();
    j["layout_count"] = LiveController::Instance().GetActiveTileCount();

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
