#include "nvr/live/live_controller.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/logger.h"
#include <algorithm>

namespace nvr {

LiveController& LiveController::Instance() {
    static LiveController instance;
    return instance;
}

LiveController::LiveController() {
    display_backend_ = std::make_shared<HeadlessDisplayBackend>();
    config_.grid_layout = 4;
    config_.max_queue_depth = 2;
    config_.drop_stale_frames = true;
}

LiveController::~LiveController() {
    Stop();
}

void LiveController::Configure(const LiveConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.grid_layout == 1) current_layout_ = LiveGridLayout::SINGLE;
    else if (config_.grid_layout == 6) current_layout_ = LiveGridLayout::GRID_6;
    else if (config_.grid_layout == 8) current_layout_ = LiveGridLayout::GRID_8;
    else current_layout_ = LiveGridLayout::GRID_4;
}

void LiveController::SetDisplayBackend(DisplayBackendPtr backend) {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    if (backend) {
        display_backend_ = backend;
    } else {
        display_backend_ = std::make_shared<HeadlessDisplayBackend>();
    }
}

DisplayBackendPtr LiveController::GetDisplayBackend() const {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    return display_backend_;
}

void LiveController::TeardownPipeline(std::shared_ptr<ChannelPipeline> pipe) {
    if (!pipe) return;
    pipe->active = false;
    pipe->packet_cv.notify_all();
    if (pipe->queue) {
        pipe->queue->Stop();
    }
    if (pipe->sub_id != 0) {
        StreamBroker::Instance().Unsubscribe(pipe->sub_id);
        pipe->sub_id = 0;
    }
    if (pipe->worker_thread.joinable()) {
        pipe->worker_thread.join();
    }
    if (pipe->stream_type == StreamType::MAIN) {
        if (!RecordingScheduler::Instance().IsChannelActive(pipe->channel_id)) {
            CameraManager::Instance().StopCamera(pipe->channel_id);
        }
    } else {
        CameraManager::Instance().StopSubStream(pipe->channel_id);
    }
}

void LiveController::TeardownPipelines(std::vector<std::shared_ptr<ChannelPipeline>>& pipes) {
    for (auto& p : pipes) {
        TeardownPipeline(p);
    }
    pipes.clear();
}

bool LiveController::SetLayout(LiveGridLayout layout) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs;
    int old_fs_id = -1;
    bool was_main = false;
    std::vector<std::shared_ptr<ChannelPipeline>> old_pipes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            old_fs_id = fullscreen_channel_id_;
            was_main = was_main_created_for_fullscreen_;
            was_main_created_for_fullscreen_ = false;
            fullscreen_channel_id_ = -1;
        }

        for (auto& [id, pipe] : pipelines_) {
            pipe->active = false;
            pipe->packet_cv.notify_all();
            if (pipe->queue) pipe->queue->Stop();
            old_pipes.push_back(std::move(pipe));
        }
        pipelines_.clear();

        current_layout_ = layout;
        previous_layout_ = layout;
        custom_tiles_.clear();
        custom_cols_ = 0;
        custom_rows_ = 0;
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
        if (was_main && old_fs_id != -1) {
            if (!RecordingScheduler::Instance().IsChannelActive(old_fs_id)) {
                CameraManager::Instance().StopCamera(old_fs_id);
            }
        }
    }
    TeardownPipelines(old_pipes);

    bool should_run = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_run = running_;
    }

    if (should_run) {
        SetupGridPipelines();
    }
    LOG_INFO << "[LiveController] Active grid layout changed to " << static_cast<int>(layout) << "-camera view";
    return true;
}

LiveGridLayout LiveController::GetCurrentLayout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fullscreen_channel_id_ != -1) {
        return LiveGridLayout::FULLSCREEN;
    }
    return current_layout_;
}

int LiveController::GetActiveTileCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fullscreen_channel_id_ != -1) return 1;
    if (current_layout_ == LiveGridLayout::CUSTOM) {
        if (!custom_tiles_.empty()) return static_cast<int>(custom_tiles_.size());
        if (custom_cols_ > 0 && custom_rows_ > 0) return custom_cols_ * custom_rows_;
        return static_cast<int>(pipelines_.size());
    }
    return static_cast<int>(current_layout_);
}

bool LiveController::SetCustomGrid(int cols, int rows, const std::vector<int>& channel_ids) {
    if (cols <= 0 || rows <= 0) return false;

    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs;
    int old_fs_id = -1;
    bool was_main = false;
    std::vector<std::shared_ptr<ChannelPipeline>> old_pipes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            old_fs_id = fullscreen_channel_id_;
            was_main = was_main_created_for_fullscreen_;
            was_main_created_for_fullscreen_ = false;
            fullscreen_channel_id_ = -1;
        }

        for (auto& [id, pipe] : pipelines_) {
            pipe->active = false;
            pipe->packet_cv.notify_all();
            if (pipe->queue) pipe->queue->Stop();
            old_pipes.push_back(std::move(pipe));
        }
        pipelines_.clear();

        current_layout_ = LiveGridLayout::CUSTOM;
        previous_layout_ = LiveGridLayout::CUSTOM;
        custom_cols_ = cols;
        custom_rows_ = rows;
        custom_tiles_.clear();

        if (!channel_ids.empty()) {
            assigned_channels_ = channel_ids;
        }
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
        if (was_main && old_fs_id != -1) {
            if (!RecordingScheduler::Instance().IsChannelActive(old_fs_id)) {
                CameraManager::Instance().StopCamera(old_fs_id);
            }
        }
    }
    TeardownPipelines(old_pipes);

    bool should_run = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_run = running_;
    }

    if (should_run) {
        SetupGridPipelines();
    }
    LOG_INFO << "[LiveController] Custom grid configured: " << cols << "x" << rows << " (" << (cols * rows) << " tiles)";
    return true;
}

bool LiveController::SetTileLayout(const std::vector<TileConfig>& tiles) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs;
    int old_fs_id = -1;
    bool was_main = false;
    std::vector<std::shared_ptr<ChannelPipeline>> old_pipes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            old_fs_id = fullscreen_channel_id_;
            was_main = was_main_created_for_fullscreen_;
            was_main_created_for_fullscreen_ = false;
            fullscreen_channel_id_ = -1;
        }

        for (auto& [id, pipe] : pipelines_) {
            pipe->active = false;
            pipe->packet_cv.notify_all();
            if (pipe->queue) pipe->queue->Stop();
            old_pipes.push_back(std::move(pipe));
        }
        pipelines_.clear();

        current_layout_ = LiveGridLayout::CUSTOM;
        previous_layout_ = LiveGridLayout::CUSTOM;
        custom_tiles_ = tiles;
        custom_cols_ = 0;
        custom_rows_ = 0;
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
        if (was_main && old_fs_id != -1) {
            if (!RecordingScheduler::Instance().IsChannelActive(old_fs_id)) {
                CameraManager::Instance().StopCamera(old_fs_id);
            }
        }
    }
    TeardownPipelines(old_pipes);

    bool should_run = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_run = running_;
    }

    if (should_run) {
        SetupGridPipelines();
    }
    LOG_INFO << "[LiveController] Custom tile layout configured with " << tiles.size() << " independent tiles";
    return true;
}

bool LiveController::ActivateChannel(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    if (!CameraManager::Instance().HasCamera(channel_id)) {
        LOG_WARN << "[LiveController] Cannot activate unknown channel " << channel_id;
        return false;
    }

    auto backend = GetDisplayBackend();
    int disp_w = backend ? backend->GetWidth() : 1920;
    int disp_h = backend ? backend->GetHeight() : 1080;
    if (disp_w <= 0) disp_w = 1920;
    if (disp_h <= 0) disp_h = 1080;

    uint32_t tw = (target_w > 0) ? target_w : static_cast<uint32_t>(disp_w / 2);
    uint32_t th = (target_h > 0) ? target_h : static_cast<uint32_t>(disp_h / 2);
    int tidx = tile_index;

    std::shared_ptr<ChannelPipeline> old_pipe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tidx < 0) {
            tidx = static_cast<int>(pipelines_.size());
        }
        auto it = pipelines_.find(channel_id);
        if (it != pipelines_.end()) {
            old_pipe = it->second;
            pipelines_.erase(it);
        }
    }

    if (old_pipe) {
        TeardownPipeline(old_pipe);
    }

    StartChannelPipeline(channel_id, tidx, stream_type, tw, th);
    LOG_INFO << "[LiveController] On-demand channel " << channel_id << " activated on tile " << tidx;
    return true;
}

bool LiveController::DeactivateChannel(int channel_id) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> pipe;
    int tidx = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pipelines_.find(channel_id);
        if (it == pipelines_.end()) {
            return false;
        }
        pipe = it->second;
        tidx = pipe->tile_index;
        pipelines_.erase(it);
    }

    if (pipe) {
        TeardownPipeline(pipe);
    }

    auto backend = GetDisplayBackend();
    if (backend && tidx >= 0) {
        backend->ClearTile(tidx);
    }
    LOG_INFO << "[LiveController] On-demand channel " << channel_id << " deactivated";
    return true;
}

bool LiveController::SetActiveChannels(const std::vector<int>& channel_ids, StreamType stream_type) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::vector<std::shared_ptr<ChannelPipeline>> to_remove;
    bool should_run = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        assigned_channels_ = channel_ids;
        should_run = running_;

        if (should_run) {
            for (auto it = pipelines_.begin(); it != pipelines_.end();) {
                if (std::find(channel_ids.begin(), channel_ids.end(), it->first) == channel_ids.end()) {
                    to_remove.push_back(it->second);
                    it = pipelines_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    TeardownPipelines(to_remove);

    if (should_run) {
        auto backend = GetDisplayBackend();
        int disp_w = backend ? backend->GetWidth() : 1920;
        int disp_h = backend ? backend->GetHeight() : 1080;
        int count = std::max(1, static_cast<int>(channel_ids.size()));
        int cols = (count == 1) ? 1 : ((count <= 4) ? 2 : ((count <= 6) ? 3 : 4));
        int rows = (count == 1) ? 1 : 2;
        uint32_t tw = static_cast<uint32_t>(disp_w / cols);
        uint32_t th = static_cast<uint32_t>(disp_h / rows);

        int tidx = 0;
        for (int id : channel_ids) {
            bool exists = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                exists = (pipelines_.find(id) != pipelines_.end());
            }
            if (!exists) {
                StartChannelPipeline(id, tidx, stream_type, tw, th);
            }
            tidx++;
        }
    }
    return true;
}

void LiveController::StopChannelPipeline(int channel_id) {
    std::shared_ptr<ChannelPipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pipelines_.find(channel_id);
        if (it != pipelines_.end()) {
            pipe = it->second;
            pipelines_.erase(it);
        }
    }
    if (pipe) {
        TeardownPipeline(pipe);
    }
}

bool LiveController::SetFullscreen(int channel_id) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    if (!CameraManager::Instance().HasCamera(channel_id)) {
        LOG_WARN << "[LiveController] Cannot enter fullscreen: channel " << channel_id << " does not exist";
        return false;
    }

    std::shared_ptr<ChannelPipeline> old_fs_pipe;
    bool was_main_created_prev = false;
    int prev_fs_id = -1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ == channel_id) {
            return true; // Already fullscreen for this channel
        }

        if (fullscreen_pipeline_) {
            old_fs_pipe = std::move(fullscreen_pipeline_);
            was_main_created_prev = was_main_created_for_fullscreen_;
            prev_fs_id = fullscreen_channel_id_;
            was_main_created_for_fullscreen_ = false;
        }

        if (fullscreen_channel_id_ == -1) {
            previous_layout_ = current_layout_;
        }
        fullscreen_channel_id_ = channel_id;

        // Pause grid SUB pipelines to preserve VPU and memory bandwidth
        for (auto& [id, pipe] : pipelines_) {
            pipe->paused = true;
        }
    }

    if (old_fs_pipe) {
        TeardownPipeline(old_fs_pipe);
        if (was_main_created_prev && prev_fs_id != -1) {
            if (!RecordingScheduler::Instance().IsChannelActive(prev_fs_id)) {
                CameraManager::Instance().StopCamera(prev_fs_id);
            }
        }
    }

    bool already_online = CameraManager::Instance().IsCameraOnline(channel_id);
    bool created_for_fs = false;
    if (already_online) {
        LOG_INFO << "[LiveController] Reusing existing active MAIN session for channel " << channel_id << " fullscreen";
    } else {
        LOG_INFO << "[LiveController] Activating on-demand MAIN session for channel " << channel_id << " fullscreen";
        CameraManager::Instance().EnsureMainStream(channel_id);
        created_for_fs = true;
    }

    auto backend = GetDisplayBackend();
    int disp_w = backend ? backend->GetWidth() : 1920;
    int disp_h = backend ? backend->GetHeight() : 1080;
    if (disp_w <= 0) disp_w = 1920;
    if (disp_h <= 0) disp_h = 1080;

    auto fs_pipe = std::make_shared<ChannelPipeline>();
    fs_pipe->channel_id = channel_id;
    fs_pipe->tile_index = 0;
    fs_pipe->stream_type = StreamType::MAIN;
    fs_pipe->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    fs_pipe->scaler = VideoScalerFactory::Create(0, 0, PixelFormat::NV12, disp_w, disp_h, PixelFormat::RGBA32);
    fs_pipe->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    fs_pipe->active = true;
    fs_pipe->paused = false;

    std::weak_ptr<ChannelPipeline> weak_pipe = fs_pipe;
    fs_pipe->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, StreamType::MAIN,
        [weak_pipe](const MediaPacketPtr& pkt) {
            auto p = weak_pipe.lock();
            if (!p || !p->active || p->paused || !pkt || pkt->IsAudio()) return;
            std::lock_guard<std::mutex> lk(p->packet_mutex);
            if (p->packet_queue.size() >= p->max_packet_depth) {
                p->packet_queue.pop_front();
            }
            p->packet_queue.push_back(pkt);
            p->packet_cv.notify_one();
        }
    );

    fs_pipe->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, fs_pipe, disp_w, disp_h);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        fullscreen_pipeline_ = fs_pipe;
        was_main_created_for_fullscreen_ = created_for_fs;
    }

    LOG_INFO << "[LiveController] Successfully switched to single-camera fullscreen (Channel " << channel_id << ")";
    return true;
}

bool LiveController::ExitFullscreen() {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs_pipe;
    bool had_main = false;
    int closing_ch = -1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ == -1) {
            return false;
        }

        closing_ch = fullscreen_channel_id_;
        old_fs_pipe = std::move(fullscreen_pipeline_);
        had_main = was_main_created_for_fullscreen_;
        was_main_created_for_fullscreen_ = false;
        fullscreen_channel_id_ = -1;
        current_layout_ = previous_layout_;

        // Resume grid pipelines
        for (auto& [id, pipe] : pipelines_) {
            pipe->paused = false;
        }
    }

    if (old_fs_pipe) {
        TeardownPipeline(old_fs_pipe);
    }

    if (had_main && closing_ch != -1) {
        if (!RecordingScheduler::Instance().IsChannelActive(closing_ch)) {
            CameraManager::Instance().StopCamera(closing_ch);
            LOG_INFO << "[LiveController] Released on-demand MAIN session for channel " << closing_ch;
        }
    }

    auto backend = GetDisplayBackend();
    if (backend) {
        backend->ClearTile(0);
    }

    LOG_INFO << "[LiveController] Exited fullscreen, restored " << static_cast<int>(current_layout_) << "-camera grid";
    return true;
}

bool LiveController::IsFullscreen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fullscreen_channel_id_ != -1;
}

int LiveController::GetFullscreenChannelId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fullscreen_channel_id_;
}

void LiveController::AssignChannels(const std::vector<int>& channel_ids) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    bool should_rebuild = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assigned_channels_ = channel_ids;
        should_rebuild = (running_ && fullscreen_channel_id_ == -1);
    }

    if (should_rebuild) {
        std::vector<std::shared_ptr<ChannelPipeline>> old_pipes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, pipe] : pipelines_) {
                pipe->active = false;
                pipe->packet_cv.notify_all();
                if (pipe->queue) pipe->queue->Stop();
                old_pipes.push_back(std::move(pipe));
            }
            pipelines_.clear();
        }
        TeardownPipelines(old_pipes);
        SetupGridPipelines();
    }
}

std::vector<int> LiveController::GetAssignedChannels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assigned_channels_;
}

void LiveController::Start() {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    bool should_setup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        should_setup = (fullscreen_channel_id_ == -1);
    }

    if (should_setup) {
        SetupGridPipelines();
    }
    LOG_INFO << "[LiveController] Started Live View pipeline engine";
}

void LiveController::Stop() {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs;
    std::vector<std::shared_ptr<ChannelPipeline>> old_pipes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;

        if (fullscreen_pipeline_) {
            old_fs = std::move(fullscreen_pipeline_);
        }

        for (auto& [id, pipe] : pipelines_) {
            pipe->active = false;
            pipe->packet_cv.notify_all();
            if (pipe->queue) pipe->queue->Stop();
            old_pipes.push_back(std::move(pipe));
        }
        pipelines_.clear();
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
    }
    TeardownPipelines(old_pipes);

    LOG_INFO << "[LiveController] Stopped Live View pipeline engine";
}

void LiveController::SetupGridPipelines() {
    auto backend = GetDisplayBackend();
    int disp_w = backend ? backend->GetWidth() : 1920;
    int disp_h = backend ? backend->GetHeight() : 1080;
    if (disp_w <= 0) disp_w = 1920;
    if (disp_h <= 0) disp_h = 1080;

    LiveGridLayout layout;
    std::vector<TileConfig> custom_tiles;
    int custom_cols = 0, custom_rows = 0;
    std::vector<int> channels_to_use;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        layout = current_layout_;
        custom_tiles = custom_tiles_;
        custom_cols = custom_cols_;
        custom_rows = custom_rows_;
        channels_to_use = assigned_channels_;
    }

    if (channels_to_use.empty()) {
        auto cams = CameraManager::Instance().GetCameras();
        for (const auto& c : cams) {
            channels_to_use.push_back(c.id);
        }
    }

    if (layout == LiveGridLayout::CUSTOM) {
        if (!custom_tiles.empty()) {
            for (const auto& t : custom_tiles) {
                uint32_t tw = (t.target_w > 0) ? t.target_w : static_cast<uint32_t>(disp_w / 2);
                uint32_t th = (t.target_h > 0) ? t.target_h : static_cast<uint32_t>(disp_h / 2);
                StartChannelPipeline(t.channel_id, t.tile_index, t.stream_type, tw, th);
            }
            return;
        }

        int cols = (custom_cols > 0) ? custom_cols : 2;
        int rows = (custom_rows > 0) ? custom_rows : 2;
        int max_tiles = cols * rows;
        uint32_t tile_w = static_cast<uint32_t>(disp_w / cols);
        uint32_t tile_h = static_cast<uint32_t>(disp_h / rows);

        int tile_idx = 0;
        for (int ch : channels_to_use) {
            if (tile_idx >= max_tiles) break;
            StartChannelPipeline(ch, tile_idx, StreamType::SUB, tile_w, tile_h);
            tile_idx++;
        }
        return;
    }

    int max_tiles = static_cast<int>(layout);
    int cols = 1, rows = 1;
    switch (layout) {
        case LiveGridLayout::SINGLE:     cols = 1; rows = 1; break;
        case LiveGridLayout::GRID_4:     cols = 2; rows = 2; break;
        case LiveGridLayout::GRID_6:     cols = 3; rows = 2; break;
        case LiveGridLayout::GRID_8:     cols = 4; rows = 2; break;
        case LiveGridLayout::FULLSCREEN: cols = 1; rows = 1; break;
        default:                         cols = 2; rows = 2; break;
    }

    uint32_t tile_w = static_cast<uint32_t>(disp_w / cols);
    uint32_t tile_h = static_cast<uint32_t>(disp_h / rows);

    int tile_idx = 0;
    for (int ch : channels_to_use) {
        if (tile_idx >= max_tiles) break;
        StartChannelPipeline(ch, tile_idx, StreamType::SUB, tile_w, tile_h);
        tile_idx++;
    }
}

void LiveController::StartChannelPipeline(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h) {
    if (stream_type == StreamType::MAIN) {
        CameraManager::Instance().EnsureMainStream(channel_id);
    } else {
        CameraManager::Instance().StartSubStream(channel_id);
    }

    auto pipe = std::make_shared<ChannelPipeline>();
    pipe->channel_id = channel_id;
    pipe->tile_index = tile_index;
    pipe->stream_type = stream_type;
    pipe->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    pipe->scaler = VideoScalerFactory::Create(0, 0, PixelFormat::NV12, target_w, target_h, PixelFormat::RGBA32);
    pipe->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    pipe->active = true;
    pipe->paused = false;

    std::weak_ptr<ChannelPipeline> weak_pipe = pipe;
    pipe->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, stream_type,
        [weak_pipe](const MediaPacketPtr& pkt) {
            auto p = weak_pipe.lock();
            if (!p || !p->active || p->paused || !pkt || pkt->IsAudio()) return;
            std::lock_guard<std::mutex> lk(p->packet_mutex);
            if (p->packet_queue.size() >= p->max_packet_depth) {
                p->packet_queue.pop_front();
            }
            p->packet_queue.push_back(pkt);
            p->packet_cv.notify_one();
        }
    );

    pipe->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, pipe, target_w, target_h);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipelines_[channel_id] = pipe;
    }
}

void LiveController::ChannelWorkerLoop(std::shared_ptr<ChannelPipeline> pipeline, uint32_t target_w, uint32_t target_h) {
    (void)target_w;
    (void)target_h;
    while (pipeline->active && running_) {
        MediaPacketPtr pkt;
        {
            std::unique_lock<std::mutex> lk(pipeline->packet_mutex);
            pipeline->packet_cv.wait_for(lk, std::chrono::milliseconds(50), [pipeline]() {
                return !pipeline->active || !pipeline->packet_queue.empty();
            });
            if (!pipeline->active) break;
            if (pipeline->paused) {
                pipeline->packet_queue.clear();
                continue;
            }
            if (!pipeline->packet_queue.empty()) {
                pkt = pipeline->packet_queue.front();
                pipeline->packet_queue.pop_front();
            }
        }

        if (!pkt) continue;

        DecodedFramePtr frame;
        if (pipeline->decoder && pipeline->decoder->Decode(pkt, frame)) {
            pipeline->queue->Push(frame);

            DecodedFramePtr live_frame;
            bool pop_ok = false;
            if (config_.drop_stale_frames) {
                pop_ok = pipeline->queue->PopLatest(live_frame);
                if (!pop_ok) {
                    pop_ok = pipeline->queue->Pop(live_frame, 10);
                }
            } else {
                pop_ok = pipeline->queue->Pop(live_frame, 10);
            }

            if (pop_ok && live_frame) {
                DecodedFramePtr scaled_frame;
                if (pipeline->scaler && pipeline->scaler->Scale(live_frame, scaled_frame)) {
                    auto backend = GetDisplayBackend();
                    if (backend) {
                        backend->RenderTile(pipeline->tile_index, pipeline->channel_id, scaled_frame);
                    }
                    pipeline->rendered_count++;

                    auto now = std::chrono::steady_clock::now();
                    if (pipeline->last_frame_time.time_since_epoch().count() > 0) {
                        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - pipeline->last_frame_time).count();
                        if (elapsed_us > 0) {
                            double instant_fps = 1000000.0 / elapsed_us;
                            pipeline->current_fps = 0.9 * pipeline->current_fps + 0.1 * instant_fps;
                        }
                    } else {
                        pipeline->current_fps = 25.0;
                    }
                    pipeline->last_frame_time = now;
                }
            }
        }
    }
}

std::vector<LiveTileMetrics> LiveController::GetTileMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LiveTileMetrics> metrics;

    if (fullscreen_channel_id_ != -1 && fullscreen_pipeline_) {
        LiveTileMetrics m;
        m.channel_id = fullscreen_pipeline_->channel_id;
        m.tile_index = 0;
        m.stream_type = StreamType::MAIN;
        m.width = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstWidth() : 1920;
        m.height = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstHeight() : 1080;
        m.enqueued_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalDropped() : 0;
        m.rendered_frames = fullscreen_pipeline_->rendered_count.load();
        m.estimated_fps = fullscreen_pipeline_->current_fps;
        m.is_fullscreen = true;
        metrics.push_back(m);
        return metrics;
    }

    for (const auto& [id, pipe] : pipelines_) {
        LiveTileMetrics m;
        m.channel_id = pipe->channel_id;
        m.tile_index = pipe->tile_index;
        m.stream_type = pipe->stream_type;
        m.width = pipe->scaler ? pipe->scaler->GetDstWidth() : 640;
        m.height = pipe->scaler ? pipe->scaler->GetDstHeight() : 360;
        m.enqueued_frames = pipe->queue ? pipe->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = pipe->queue ? pipe->queue->GetTotalDropped() : 0;
        m.rendered_frames = pipe->rendered_count.load();
        m.estimated_fps = pipe->current_fps;
        m.is_fullscreen = false;
        metrics.push_back(m);
    }
    return metrics;
}

LiveTileMetrics LiveController::GetChannelMetrics(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    LiveTileMetrics m;
    m.channel_id = channel_id;

    if (fullscreen_channel_id_ == channel_id && fullscreen_pipeline_) {
        m.tile_index = 0;
        m.stream_type = StreamType::MAIN;
        m.width = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstWidth() : 1920;
        m.height = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstHeight() : 1080;
        m.enqueued_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalDropped() : 0;
        m.rendered_frames = fullscreen_pipeline_->rendered_count.load();
        m.estimated_fps = fullscreen_pipeline_->current_fps;
        m.is_fullscreen = true;
        return m;
    }

    auto it = pipelines_.find(channel_id);
    if (it != pipelines_.end()) {
        const auto& pipe = it->second;
        m.tile_index = pipe->tile_index;
        m.stream_type = pipe->stream_type;
        m.width = pipe->scaler ? pipe->scaler->GetDstWidth() : 640;
        m.height = pipe->scaler ? pipe->scaler->GetDstHeight() : 360;
        m.enqueued_frames = pipe->queue ? pipe->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = pipe->queue ? pipe->queue->GetTotalDropped() : 0;
        m.rendered_frames = pipe->rendered_count.load();
        m.estimated_fps = pipe->current_fps;
        m.is_fullscreen = false;
    }
    return m;
}

} // namespace nvr
