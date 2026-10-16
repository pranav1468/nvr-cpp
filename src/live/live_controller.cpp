#include "nvr/live/live_controller.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/logger.h"
#include <algorithm>
#include <unordered_set>

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
    {
        std::lock_guard<std::mutex> cb_lock(pipe->callback_mutex);
        pipe->on_first_frame = nullptr;
    }
    if (pipe->worker_thread.joinable()) {
        pipe->worker_thread.join();
    }
    if (pipe->stream_type == StreamType::MAIN) {
        CameraManager::Instance().StopCamera(pipe->channel_id, false);
    } else {
        CameraManager::Instance().StopSubStream(pipe->channel_id, false);
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            fullscreen_channel_id_ = -1;
        }

        current_layout_ = layout;
        previous_layout_ = layout;
        custom_tiles_.clear();
        custom_cols_ = 0;
        custom_rows_ = 0;
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
    }

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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            fullscreen_channel_id_ = -1;
        }

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
    }

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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ != -1) {
            old_fs = std::move(fullscreen_pipeline_);
            fullscreen_channel_id_ = -1;
        }

        current_layout_ = LiveGridLayout::CUSTOM;
        previous_layout_ = LiveGridLayout::CUSTOM;
        custom_tiles_ = tiles;
        custom_cols_ = 0;
        custom_rows_ = 0;
    }

    if (old_fs) {
        TeardownPipeline(old_fs);
    }

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

bool LiveController::ActivateChannel(int channel_id, int tile_index, StreamType stream_type, uint32_t /*target_w*/, uint32_t /*target_h*/) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    if (!CameraManager::Instance().HasCamera(channel_id)) {
        LOG_WARN << "[LiveController] Cannot activate unknown channel " << channel_id;
        return false;
    }

    int tidx = tile_index;

    std::shared_ptr<ChannelPipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tidx < 0) {
            tidx = static_cast<int>(pipelines_.size());
        }
        auto it = pipelines_.find(channel_id);
        if (it != pipelines_.end()) {
            pipe = it->second;
        }
    }

    if (pipe && pipe->stream_type == stream_type) {
        if (pipe->paused.exchange(false)) {
            pipe->awaiting_keyframe.store(true);
        }
        std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
        bool found = false;
        for (const auto& t : pipe->targets) {
            if (t.tile_index == tidx) {
                found = true;
                break;
            }
        }
        if (!found) {
            TileTarget tt;
            tt.tile_index = tidx;
            pipe->targets.push_back(std::move(tt));
        }
        LOG_INFO << "[LiveController] On-demand channel " << channel_id << " target updated on tile " << tidx;
        return true;
    }

    StartChannelPipeline(channel_id, tidx, stream_type, 0, 0);
    LOG_INFO << "[LiveController] On-demand channel " << channel_id << " activated on tile " << tidx;
    return true;
}

bool LiveController::DeactivateChannel(int channel_id) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pipelines_.find(channel_id);
        if (it == pipelines_.end()) {
            return false;
        }
        pipe = it->second;
        pipelines_.erase(it);
    }

    std::vector<int> old_tiles;
    if (pipe) {
        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            for (const auto& t : pipe->targets) {
                old_tiles.push_back(t.tile_index);
            }
        }
        TeardownPipeline(pipe);
    }

    auto backend = GetDisplayBackend();
    if (backend) {
        for (int t : old_tiles) {
            if (t >= 0) backend->ClearTile(t);
        }
    }
    LOG_INFO << "[LiveController] On-demand channel " << channel_id << " deactivated";
    return true;
}

bool LiveController::SetActiveChannels(const std::vector<int>& channel_ids, StreamType stream_type) {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    bool should_run = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assigned_channels_ = channel_ids;
        should_run = running_;
    }

    if (should_run) {
        std::vector<TileConfig> desired_tiles;
        int tidx = 0;
        for (int id : channel_ids) {
            TileConfig dt;
            dt.tile_index = tidx++;
            dt.channel_id = id;
            dt.stream_type = stream_type;
            desired_tiles.push_back(dt);
        }

        ReconcilePipelines(desired_tiles);
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ == channel_id) {
            return true; // Already fullscreen for this channel
        }

        if (fullscreen_pipeline_) {
            old_fs_pipe = std::move(fullscreen_pipeline_);
        }

        if (fullscreen_channel_id_ == -1) {
            previous_layout_ = current_layout_;
        }
        fullscreen_channel_id_ = channel_id;

        // Seamless handoff: if channel is active in grid, retarget to tile 0 immediately
        auto it = pipelines_.find(channel_id);
        if (it != pipelines_.end()) {
            if (it->second->paused.exchange(false)) {
                it->second->awaiting_keyframe.store(true);
            }
            TileTarget tt;
            tt.tile_index = 0;
            std::lock_guard<std::mutex> t_lock(it->second->targets_mutex);
            it->second->targets.clear();
            it->second->targets.push_back(std::move(tt));

            // Instant Continuity: seed fullscreen tile 0 immediately with last decoded frame
            DecodedFramePtr seed_frame;
            {
                std::lock_guard<std::mutex> f_lock(it->second->frame_mutex);
                seed_frame = it->second->last_rendered_frame;
            }
            auto backend = GetDisplayBackend();
            if (backend && seed_frame) {
                backend->RenderTile(0, channel_id, seed_frame);
            }
        }

        // Pause other grid SUB pipelines to preserve VPU and memory bandwidth
        for (auto& [id, pipe] : pipelines_) {
            if (id != channel_id) {
                pipe->paused.store(true);
            }
        }
    }

    if (old_fs_pipe) {
        TeardownPipeline(old_fs_pipe);
    }

    bool already_online = CameraManager::Instance().IsCameraOnline(channel_id);
    if (already_online) {
        LOG_INFO << "[LiveController] Reusing existing active MAIN session for channel " << channel_id << " fullscreen";
    } else {
        LOG_INFO << "[LiveController] Activating on-demand MAIN session for channel " << channel_id << " fullscreen";
        CameraManager::Instance().EnsureMainStream(channel_id);
    }

    auto fs_pipe = std::make_shared<ChannelPipeline>();
    fs_pipe->channel_id = channel_id;
    fs_pipe->stream_type = StreamType::MAIN;
    fs_pipe->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    fs_pipe->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    fs_pipe->active = true;
    fs_pipe->paused = false;
    fs_pipe->awaiting_keyframe = true;
    {
        TileTarget tt;
        tt.tile_index = 0;
        fs_pipe->targets.push_back(std::move(tt));
    }

    // When MAIN stream delivers its first decoded frame, pause the SUB stream to free VPU capacity
    fs_pipe->on_first_frame = [this, channel_id]() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ == channel_id) {
            auto it = pipelines_.find(channel_id);
            if (it != pipelines_.end()) {
                auto& sub_pipe = it->second;
                sub_pipe->paused.store(true);
                {
                    std::lock_guard<std::mutex> lk(sub_pipe->packet_mutex);
                    sub_pipe->packet_queue.clear();
                }
                if (sub_pipe->queue) {
                    sub_pipe->queue->Clear();
                }
                LOG_INFO << "[LiveController] MAIN stream first frame rendered for channel "
                         << channel_id << "; paused SUB stream and flushed pending queue";
            }
        }
    };

    std::weak_ptr<ChannelPipeline> weak_pipe = fs_pipe;
    fs_pipe->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, StreamType::MAIN,
        [weak_pipe](const MediaPacketPtr& pkt) {
            auto p = weak_pipe.lock();
            if (!p || !p->active || p->paused || !pkt || pkt->IsAudio()) return;

            // Ingress admission gating: if awaiting keyframe, discard non-keyframes immediately
            // without taking the mutex or waking the worker thread.
            if (p->awaiting_keyframe.load(std::memory_order_relaxed) && !pkt->is_keyframe) {
                return;
            }

            std::lock_guard<std::mutex> lk(p->packet_mutex);
            if (p->packet_queue.size() >= p->max_packet_depth) {
                p->packet_queue.clear();
                p->awaiting_keyframe.store(true, std::memory_order_release);
                if (!pkt->is_keyframe) {
                    return;
                }
            }
            p->packet_queue.push_back(pkt);
            p->packet_cv.notify_one();
        }
    );

    fs_pipe->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, fs_pipe);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        fullscreen_pipeline_ = fs_pipe;
    }

    LOG_INFO << "[LiveController] Successfully switched to single-camera fullscreen (Channel " << channel_id << ")";
    return true;
}

bool LiveController::ExitFullscreen() {
    std::unique_lock<std::mutex> layout_lock(layout_mutex_);

    std::shared_ptr<ChannelPipeline> old_fs_pipe;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullscreen_channel_id_ == -1) {
            return false;
        }

        old_fs_pipe = std::move(fullscreen_pipeline_);
        fullscreen_channel_id_ = -1;
        current_layout_ = previous_layout_;
    }

    if (old_fs_pipe) {
        TeardownPipeline(old_fs_pipe);
    }

    bool should_run = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_run = running_;
    }

    if (should_run) {
        SetupGridPipelines();
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

    std::vector<TileConfig> desired_tiles;

    if (layout == LiveGridLayout::CUSTOM) {
        if (!custom_tiles.empty()) {
            desired_tiles = custom_tiles;
        } else {
            int cols = (custom_cols > 0) ? custom_cols : 2;
            int rows = (custom_rows > 0) ? custom_rows : 2;
            int max_tiles = cols * rows;

            int tile_idx = 0;
            for (int ch : channels_to_use) {
                if (tile_idx >= max_tiles) break;
                TileConfig dt;
                dt.tile_index = tile_idx++;
                dt.channel_id = ch;
                dt.stream_type = StreamType::SUB;
                desired_tiles.push_back(dt);
            }
        }
    } else {
        int max_tiles = static_cast<int>(layout);

        // Deduplicate channels for preset grids so distinct cameras fill each tile
        std::vector<int> distinct_channels;
        std::unordered_set<int> seen_channels;
        for (int ch : channels_to_use) {
            if (seen_channels.insert(ch).second) {
                distinct_channels.push_back(ch);
            }
        }

        int tile_idx = 0;
        for (int ch : distinct_channels) {
            if (tile_idx >= max_tiles) break;
            TileConfig dt;
            dt.tile_index = tile_idx++;
            dt.channel_id = ch;
            dt.stream_type = StreamType::SUB;
            desired_tiles.push_back(dt);
        }
    }

    ReconcilePipelines(desired_tiles);
}

void LiveController::ReconcilePipelines(const std::vector<TileConfig>& desired_tiles) {
    // Group desired tiles by channel_id
    std::unordered_map<int, std::vector<TileConfig>> desired_by_channel;
    std::unordered_set<int> desired_tile_indices;
    for (const auto& dt : desired_tiles) {
        desired_by_channel[dt.channel_id].push_back(dt);
        desired_tile_indices.insert(dt.tile_index);
    }

    std::vector<std::shared_ptr<ChannelPipeline>> to_remove;
    std::vector<std::pair<std::shared_ptr<ChannelPipeline>, std::vector<TileConfig>>> to_update;
    std::vector<std::pair<int, std::vector<TileConfig>>> to_add;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pipelines_.begin(); it != pipelines_.end();) {
            auto fit = desired_by_channel.find(it->first);
            if (fit == desired_by_channel.end()) {
                to_remove.push_back(it->second);
                it = pipelines_.erase(it);
            } else if (it->second->stream_type != fit->second.front().stream_type) {
                to_remove.push_back(it->second);
                it = pipelines_.erase(it);
                to_add.push_back({fit->first, fit->second});
            } else {
                to_update.push_back({it->second, fit->second});
                ++it;
            }
        }

        for (const auto& [ch, d_tiles] : desired_by_channel) {
            if (pipelines_.find(ch) == pipelines_.end()) {
                bool already_in_add = false;
                for (const auto& a : to_add) {
                    if (a.first == ch) {
                        already_in_add = true;
                        break;
                    }
                }
                if (!already_in_add) {
                    to_add.push_back({ch, d_tiles});
                }
            }
        }
    }

    // 1. Teardown removed pipelines outside mutex_
    for (auto& pipe : to_remove) {
        std::vector<int> old_tiles;
        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            for (const auto& t : pipe->targets) {
                old_tiles.push_back(t.tile_index);
            }
        }
        TeardownPipeline(pipe);
        auto backend = GetDisplayBackend();
        if (backend) {
            for (int old_tile : old_tiles) {
                if (desired_tile_indices.find(old_tile) == desired_tile_indices.end()) {
                    backend->ClearTile(old_tile);
                }
            }
        }
    }

    // 2. In-place update kept pipelines without restarting threads or decoders
    auto backend = GetDisplayBackend();
    for (auto& [pipe, d_tiles] : to_update) {
        bool was_paused = pipe->paused.exchange(false);
        if (was_paused) {
            pipe->awaiting_keyframe.store(true);
        }

        std::vector<int> old_indices;
        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            for (const auto& t : pipe->targets) {
                old_indices.push_back(t.tile_index);
            }
        }

        std::vector<TileTarget> new_targets;
        std::vector<int> newly_assigned_tiles;
        for (const auto& dt : d_tiles) {
            TileTarget tt;
            tt.tile_index = dt.tile_index;
            new_targets.push_back(std::move(tt));
            if (std::find(old_indices.begin(), old_indices.end(), dt.tile_index) == old_indices.end()) {
                newly_assigned_tiles.push_back(dt.tile_index);
            }
        }

        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            pipe->targets = std::move(new_targets);
        }

        // Instant Continuity: seed newly assigned or unpaused tiles immediately with the channel's current frame
        if (backend) {
            DecodedFramePtr seed_frame;
            {
                std::lock_guard<std::mutex> f_lock(pipe->frame_mutex);
                seed_frame = pipe->last_rendered_frame;
            }
            if (seed_frame) {
                if (was_paused) {
                    for (const auto& dt : d_tiles) {
                        backend->RenderTile(dt.tile_index, pipe->channel_id, seed_frame);
                    }
                } else {
                    for (int new_tidx : newly_assigned_tiles) {
                        backend->RenderTile(new_tidx, pipe->channel_id, seed_frame);
                    }
                }
            }
        }
    }

    // 3. Start added pipelines
    for (const auto& [ch, d_tiles] : to_add) {
        if (d_tiles.empty()) continue;
        StartChannelPipelineMulti(ch, d_tiles.front().stream_type, d_tiles);
    }
}

void LiveController::StartChannelPipeline(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h) {
    TileConfig tc;
    tc.tile_index = tile_index;
    tc.channel_id = channel_id;
    tc.stream_type = stream_type;
    tc.target_w = target_w;
    tc.target_h = target_h;
    StartChannelPipelineMulti(channel_id, stream_type, {tc});
}

void LiveController::StartChannelPipelineMulti(int channel_id, StreamType stream_type, const std::vector<TileConfig>& targets) {
    std::shared_ptr<ChannelPipeline> old_pipe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pipelines_.find(channel_id);
        if (it != pipelines_.end()) {
            old_pipe = it->second;
            pipelines_.erase(it);
        }
    }
    if (old_pipe) {
        TeardownPipeline(old_pipe);
    }

    if (stream_type == StreamType::MAIN) {
        CameraManager::Instance().EnsureMainStream(channel_id);
    } else {
        CameraManager::Instance().StartSubStream(channel_id);
    }

    auto pipe = std::make_shared<ChannelPipeline>();
    pipe->channel_id = channel_id;
    pipe->stream_type = stream_type;
    pipe->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    pipe->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    pipe->active = true;
    pipe->paused = false;
    pipe->awaiting_keyframe = true;

    for (const auto& t : targets) {
        TileTarget tt;
        tt.tile_index = t.tile_index;
        pipe->targets.push_back(std::move(tt));
    }

    std::weak_ptr<ChannelPipeline> weak_pipe = pipe;
    pipe->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, stream_type,
        [weak_pipe](const MediaPacketPtr& pkt) {
            auto p = weak_pipe.lock();
            if (!p || !p->active || p->paused || !pkt || pkt->IsAudio()) return;

            // Ingress admission gating: if awaiting keyframe, discard non-keyframes immediately
            // without taking the mutex or waking the worker thread.
            if (p->awaiting_keyframe.load(std::memory_order_relaxed) && !pkt->is_keyframe) {
                return;
            }

            std::lock_guard<std::mutex> lk(p->packet_mutex);
            if (p->packet_queue.size() >= p->max_packet_depth) {
                p->packet_queue.clear();
                p->awaiting_keyframe.store(true, std::memory_order_release);
                if (!pkt->is_keyframe) {
                    return;
                }
            }
            p->packet_queue.push_back(pkt);
            p->packet_cv.notify_one();
        }
    );

    pipe->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, pipe);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipelines_[channel_id] = pipe;
    }
}

void LiveController::ChannelWorkerLoop(std::shared_ptr<ChannelPipeline> pipeline) {
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

        // L7 Resynchronization: discard non-keyframes when dependency chain is broken
        if (pipeline->awaiting_keyframe.load()) {
            if (!pkt->is_keyframe) {
                continue;
            }
            if (pipeline->decoder) {
                pipeline->decoder->Flush();
            }
            pipeline->awaiting_keyframe.store(false);
            LOG_INFO << "[LiveController] Channel " << pipeline->channel_id
                     << " (" << (pipeline->stream_type == StreamType::MAIN ? "MAIN" : "SUB")
                     << ") resynchronized to keyframe; resumed decoding";
        }

        DecodedFramePtr frame;
        if (pipeline->decoder) {
            bool decode_ok = pipeline->decoder->Decode(pkt, frame);
            if (!decode_ok) {
                pipeline->awaiting_keyframe.store(true);
                pipeline->decoder->Flush();
                continue;
            }
        }

        if (frame) {
            // Discard frame if pipeline is paused to avoid stale overwrites
            if (pipeline->paused.load(std::memory_order_relaxed)) {
                if (pipeline->queue) {
                    pipeline->queue->RecordDirect(true);
                }
                continue;
            }

            if (pipeline->queue) {
                pipeline->queue->RecordDirect(false);
            }

            // Record native stream resolution
            pipeline->stream_width.store(frame->width, std::memory_order_relaxed);
            pipeline->stream_height.store(frame->height, std::memory_order_relaxed);

            std::vector<TileTarget> active_targets;
            {
                std::lock_guard<std::mutex> t_lock(pipeline->targets_mutex);
                active_targets = pipeline->targets;
            }

            if (active_targets.empty()) {
                continue;
            }

            auto backend = GetDisplayBackend();
            bool any_rendered = false;

            for (const auto& target : active_targets) {
                if (backend) {
                    backend->RenderTile(target.tile_index, pipeline->channel_id, frame);
                }
                any_rendered = true;
            }

            if (any_rendered) {
                pipeline->rendered_count.fetch_add(1, std::memory_order_relaxed);

                {
                    std::lock_guard<std::mutex> f_lock(pipeline->frame_mutex);
                    pipeline->last_rendered_frame = frame;
                }

                if (!pipeline->first_frame_rendered.exchange(true)) {
                    std::function<void()> cb;
                    {
                        std::lock_guard<std::mutex> cb_lock(pipeline->callback_mutex);
                        cb = pipeline->on_first_frame;
                    }
                    if (cb) {
                        cb();
                    }
                }

                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                int64_t prev_ms = pipeline->last_frame_time_ms.exchange(now_ms, std::memory_order_relaxed);
                if (prev_ms > 0) {
                    auto elapsed_ms = now_ms - prev_ms;
                    if (elapsed_ms > 0) {
                        double instant_fps = 1000.0 / elapsed_ms;
                        pipeline->current_fps = 0.9 * pipeline->current_fps + 0.1 * instant_fps;
                    }
                } else {
                    pipeline->current_fps = 25.0;
                }
            }
        }
    }
}

static TileConnectionState ComputeTileState(bool is_online, uint64_t rendered_count, int64_t last_frame_time_ms) {
    if (!is_online) {
        return TileConnectionState::OFFLINE;
    }
    if (rendered_count == 0 || last_frame_time_ms == 0) {
        return TileConnectionState::CONNECTING;
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto elapsed = now_ms - last_frame_time_ms;
    if (elapsed > 5000) {
        return TileConnectionState::STALLED;
    }
    return TileConnectionState::STREAMING;
}

std::vector<LiveTileMetrics> LiveController::GetTileMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LiveTileMetrics> metrics;

    if (fullscreen_channel_id_ != -1 && fullscreen_pipeline_) {
        LiveTileMetrics m;
        m.channel_id = fullscreen_pipeline_->channel_id;
        m.tile_index = 0;
        m.stream_type = StreamType::MAIN;
        uint32_t sw = fullscreen_pipeline_->stream_width.load();
        uint32_t sh = fullscreen_pipeline_->stream_height.load();
        m.width = (sw > 0) ? sw : 1920;
        m.height = (sh > 0) ? sh : 1080;
        m.enqueued_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalDropped() : 0;
        m.rendered_frames = fullscreen_pipeline_->rendered_count.load();
        m.estimated_fps = fullscreen_pipeline_->current_fps;
        m.is_fullscreen = true;
        bool is_online = CameraManager::Instance().IsCameraOnline(fullscreen_pipeline_->channel_id);
        m.state = ComputeTileState(is_online, m.rendered_frames, fullscreen_pipeline_->last_frame_time_ms.load(std::memory_order_relaxed));
        metrics.push_back(m);
        return metrics;
    }

    for (const auto& [id, pipe] : pipelines_) {
        bool is_online = (pipe->stream_type == StreamType::MAIN) ?
                         CameraManager::Instance().IsCameraOnline(pipe->channel_id) :
                         CameraManager::Instance().IsSubStreamOnline(pipe->channel_id);
        uint64_t rendered = pipe->rendered_count.load();
        TileConnectionState state = ComputeTileState(is_online, rendered, pipe->last_frame_time_ms.load(std::memory_order_relaxed));

        std::vector<TileTarget> active_targets;
        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            active_targets = pipe->targets;
        }

        uint32_t sw = pipe->stream_width.load();
        uint32_t sh = pipe->stream_height.load();
        uint32_t def_w = (pipe->stream_type == StreamType::MAIN) ? 1920 : 640;
        uint32_t def_h = (pipe->stream_type == StreamType::MAIN) ? 1080 : 360;
        uint32_t actual_w = (sw > 0) ? sw : def_w;
        uint32_t actual_h = (sh > 0) ? sh : def_h;

        if (active_targets.empty()) {
            LiveTileMetrics m;
            m.channel_id = pipe->channel_id;
            m.tile_index = -1;
            m.stream_type = pipe->stream_type;
            m.width = actual_w;
            m.height = actual_h;
            m.enqueued_frames = pipe->queue ? pipe->queue->GetTotalEnqueued() : 0;
            m.dropped_frames = pipe->queue ? pipe->queue->GetTotalDropped() : 0;
            m.rendered_frames = rendered;
            m.estimated_fps = pipe->current_fps;
            m.is_fullscreen = false;
            m.state = state;
            metrics.push_back(m);
        } else {
            for (const auto& target : active_targets) {
                LiveTileMetrics m;
                m.channel_id = pipe->channel_id;
                m.tile_index = target.tile_index;
                m.stream_type = pipe->stream_type;
                m.width = actual_w;
                m.height = actual_h;
                m.enqueued_frames = pipe->queue ? pipe->queue->GetTotalEnqueued() : 0;
                m.dropped_frames = pipe->queue ? pipe->queue->GetTotalDropped() : 0;
                m.rendered_frames = rendered;
                m.estimated_fps = pipe->current_fps;
                m.is_fullscreen = false;
                m.state = state;
                metrics.push_back(m);
            }
        }
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
        uint32_t sw = fullscreen_pipeline_->stream_width.load();
        uint32_t sh = fullscreen_pipeline_->stream_height.load();
        m.width = (sw > 0) ? sw : 1920;
        m.height = (sh > 0) ? sh : 1080;
        m.enqueued_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = fullscreen_pipeline_->queue ? fullscreen_pipeline_->queue->GetTotalDropped() : 0;
        m.rendered_frames = fullscreen_pipeline_->rendered_count.load();
        m.estimated_fps = fullscreen_pipeline_->current_fps;
        m.is_fullscreen = true;
        bool is_online = CameraManager::Instance().IsCameraOnline(channel_id);
        m.state = ComputeTileState(is_online, m.rendered_frames, fullscreen_pipeline_->last_frame_time_ms.load(std::memory_order_relaxed));
        return m;
    }

    auto it = pipelines_.find(channel_id);
    if (it != pipelines_.end()) {
        const auto& pipe = it->second;
        TileTarget first_target;
        bool has_target = false;
        {
            std::lock_guard<std::mutex> t_lock(pipe->targets_mutex);
            if (!pipe->targets.empty()) {
                first_target = pipe->targets.front();
                has_target = true;
            }
        }
        uint32_t sw = pipe->stream_width.load();
        uint32_t sh = pipe->stream_height.load();
        uint32_t def_w = (pipe->stream_type == StreamType::MAIN) ? 1920 : 640;
        uint32_t def_h = (pipe->stream_type == StreamType::MAIN) ? 1080 : 360;
        m.tile_index = has_target ? first_target.tile_index : 0;
        m.stream_type = pipe->stream_type;
        m.width = (sw > 0) ? sw : def_w;
        m.height = (sh > 0) ? sh : def_h;
        m.enqueued_frames = pipe->queue ? pipe->queue->GetTotalEnqueued() : 0;
        m.dropped_frames = pipe->queue ? pipe->queue->GetTotalDropped() : 0;
        m.rendered_frames = pipe->rendered_count.load();
        m.estimated_fps = pipe->current_fps;
        m.is_fullscreen = false;
        bool is_online = (pipe->stream_type == StreamType::MAIN) ?
                         CameraManager::Instance().IsCameraOnline(channel_id) :
                         CameraManager::Instance().IsSubStreamOnline(channel_id);
        m.state = ComputeTileState(is_online, m.rendered_frames, pipe->last_frame_time_ms.load(std::memory_order_relaxed));
    }
    return m;
}

} // namespace nvr
