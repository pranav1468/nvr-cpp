#include "nvr/live/live_controller.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/logger.h"

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
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend) {
        display_backend_ = backend;
    } else {
        display_backend_ = std::make_shared<HeadlessDisplayBackend>();
    }
}

DisplayBackendPtr LiveController::GetDisplayBackend() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return display_backend_;
}

bool LiveController::SetLayout(LiveGridLayout layout) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fullscreen_channel_id_ != -1) {
        // Automatically exit fullscreen if changing layout
        if (fullscreen_pipeline_) {
            fullscreen_pipeline_->active = false;
            if (fullscreen_pipeline_->queue) fullscreen_pipeline_->queue->Stop();
            if (fullscreen_pipeline_->sub_id != 0) {
                StreamBroker::Instance().Unsubscribe(fullscreen_pipeline_->sub_id);
            }
            if (fullscreen_pipeline_->worker_thread.joinable()) {
                fullscreen_pipeline_->worker_thread.join();
            }
            fullscreen_pipeline_.reset();
        }

        if (was_main_created_for_fullscreen_) {
            if (!RecordingScheduler::Instance().IsChannelRecording(fullscreen_channel_id_)) {
                CameraManager::Instance().StopCamera(fullscreen_channel_id_);
            }
            was_main_created_for_fullscreen_ = false;
        }
        fullscreen_channel_id_ = -1;
    }

    current_layout_ = layout;
    previous_layout_ = layout;

    if (running_) {
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
    return static_cast<int>(current_layout_);
}

bool LiveController::SetFullscreen(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CameraManager::Instance().HasCamera(channel_id)) {
        LOG_WARN << "[LiveController] Cannot enter fullscreen: channel " << channel_id << " does not exist";
        return false;
    }

    if (fullscreen_channel_id_ == channel_id) {
        return true; // Already fullscreen for this channel
    }

    // Clean up previous fullscreen if any
    if (fullscreen_pipeline_) {
        fullscreen_pipeline_->active = false;
        if (fullscreen_pipeline_->queue) fullscreen_pipeline_->queue->Stop();
        if (fullscreen_pipeline_->sub_id != 0) {
            StreamBroker::Instance().Unsubscribe(fullscreen_pipeline_->sub_id);
        }
        if (fullscreen_pipeline_->worker_thread.joinable()) {
            fullscreen_pipeline_->worker_thread.join();
        }
        fullscreen_pipeline_.reset();

        if (was_main_created_for_fullscreen_) {
            if (!RecordingScheduler::Instance().IsChannelActive(fullscreen_channel_id_)) {
                CameraManager::Instance().StopCamera(fullscreen_channel_id_);
            }
            was_main_created_for_fullscreen_ = false;
        }
    }

    if (fullscreen_channel_id_ == -1) {
        previous_layout_ = current_layout_;
    }
    fullscreen_channel_id_ = channel_id;

    // Check if MAIN session is already active (e.g. for recording) -> REUSE IT!
    bool already_online = CameraManager::Instance().IsCameraOnline(channel_id);
    if (already_online) {
        LOG_INFO << "[LiveController] Reusing existing active MAIN session for channel " << channel_id << " fullscreen";
        was_main_created_for_fullscreen_ = false;
    } else {
        LOG_INFO << "[LiveController] Activating on-demand MAIN session for channel " << channel_id << " fullscreen";
        CameraManager::Instance().EnsureMainStream(channel_id);
        was_main_created_for_fullscreen_ = true;
    }

    // Pause grid SUB pipelines to preserve VPU and memory bandwidth
    for (auto& [id, pipe] : pipelines_) {
        pipe->paused = true;
    }

    int disp_w = display_backend_ ? display_backend_->GetWidth() : 1920;
    int disp_h = display_backend_ ? display_backend_->GetHeight() : 1080;
    if (disp_w <= 0) disp_w = 1920;
    if (disp_h <= 0) disp_h = 1080;

    // Set up single-camera fullscreen pipeline (dynamic display dimensions)
    fullscreen_pipeline_ = std::make_unique<ChannelPipeline>();
    fullscreen_pipeline_->channel_id = channel_id;
    fullscreen_pipeline_->tile_index = 0;
    fullscreen_pipeline_->stream_type = StreamType::MAIN;
    fullscreen_pipeline_->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    fullscreen_pipeline_->scaler = VideoScalerFactory::Create(0, 0, PixelFormat::NV12, disp_w, disp_h, PixelFormat::RGBA32);
    fullscreen_pipeline_->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    fullscreen_pipeline_->active = true;
    fullscreen_pipeline_->paused = false;

    ChannelPipeline* raw_pipe = fullscreen_pipeline_.get();
    fullscreen_pipeline_->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, StreamType::MAIN,
        [raw_pipe](const MediaPacketPtr& pkt) {
            if (!raw_pipe->active || raw_pipe->paused || !pkt || pkt->IsAudio()) return;
            std::lock_guard<std::mutex> lk(raw_pipe->packet_mutex);
            if (raw_pipe->packet_queue.size() >= raw_pipe->max_packet_depth) {
                raw_pipe->packet_queue.pop_front();
            }
            raw_pipe->packet_queue.push_back(pkt);
            raw_pipe->packet_cv.notify_one();
        }
    );

    fullscreen_pipeline_->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, raw_pipe, disp_w, disp_h);
    LOG_INFO << "[LiveController] Successfully switched to single-camera fullscreen (Channel " << channel_id << ")";
    return true;
}

bool LiveController::ExitFullscreen() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fullscreen_channel_id_ == -1) {
        return false;
    }

    int closing_channel = fullscreen_channel_id_;
    if (fullscreen_pipeline_) {
        fullscreen_pipeline_->active = false;
        fullscreen_pipeline_->packet_cv.notify_all();
        if (fullscreen_pipeline_->queue) fullscreen_pipeline_->queue->Stop();
        if (fullscreen_pipeline_->sub_id != 0) {
            StreamBroker::Instance().Unsubscribe(fullscreen_pipeline_->sub_id);
        }
        if (fullscreen_pipeline_->worker_thread.joinable()) {
            fullscreen_pipeline_->worker_thread.join();
        }
        fullscreen_pipeline_.reset();
    }

    if (!RecordingScheduler::Instance().IsChannelActive(closing_channel)) {
        CameraManager::Instance().StopCamera(closing_channel);
        LOG_INFO << "[LiveController] Released on-demand MAIN session for channel " << closing_channel;
    }
    was_main_created_for_fullscreen_ = false;

    fullscreen_channel_id_ = -1;
    current_layout_ = previous_layout_;

    // Resume grid pipelines
    for (auto& [id, pipe] : pipelines_) {
        pipe->paused = false;
    }

    if (display_backend_) {
        display_backend_->ClearTile(0);
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
    std::lock_guard<std::mutex> lock(mutex_);
    assigned_channels_ = channel_ids;
    if (running_ && fullscreen_channel_id_ == -1) {
        SetupGridPipelines();
    }
}

std::vector<int> LiveController::GetAssignedChannels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assigned_channels_;
}

void LiveController::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    if (fullscreen_channel_id_ == -1) {
        SetupGridPipelines();
    }
    LOG_INFO << "[LiveController] Started Live View pipeline engine";
}

void LiveController::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    running_ = false;

    if (fullscreen_pipeline_) {
        fullscreen_pipeline_->active = false;
        if (fullscreen_pipeline_->queue) fullscreen_pipeline_->queue->Stop();
        if (fullscreen_pipeline_->sub_id != 0) {
            StreamBroker::Instance().Unsubscribe(fullscreen_pipeline_->sub_id);
        }
        if (fullscreen_pipeline_->worker_thread.joinable()) {
            fullscreen_pipeline_->worker_thread.join();
        }
        fullscreen_pipeline_.reset();
    }

    TeardownGridPipelines();
    LOG_INFO << "[LiveController] Stopped Live View pipeline engine";
}

void LiveController::SetupGridPipelines() {
    TeardownGridPipelines();

    int max_tiles = static_cast<int>(current_layout_);
    std::vector<int> channels_to_use = assigned_channels_;

    // Fallback: if no specific assignment, use configured cameras from CameraManager
    if (channels_to_use.empty()) {
        auto cams = CameraManager::Instance().GetCameras();
        for (const auto& c : cams) {
            channels_to_use.push_back(c.id);
        }
    }

    int disp_w = display_backend_ ? display_backend_->GetWidth() : 1920;
    int disp_h = display_backend_ ? display_backend_->GetHeight() : 1080;
    if (disp_w <= 0) disp_w = 1920;
    if (disp_h <= 0) disp_h = 1080;

    int cols = 1, rows = 1;
    switch (current_layout_) {
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

void LiveController::TeardownGridPipelines() {
    for (auto& [id, pipe] : pipelines_) {
        pipe->active = false;
        pipe->packet_cv.notify_all();
        if (pipe->queue) pipe->queue->Stop();
        if (pipe->sub_id != 0) {
            StreamBroker::Instance().Unsubscribe(pipe->sub_id);
        }
        if (pipe->worker_thread.joinable()) {
            pipe->worker_thread.join();
        }
        CameraManager::Instance().StopSubStream(id);
    }
    pipelines_.clear();
}

void LiveController::StartChannelPipeline(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h) {
    // Start SUB session via CameraManager
    CameraManager::Instance().StartSubStream(channel_id);

    auto pipe = std::make_unique<ChannelPipeline>();
    pipe->channel_id = channel_id;
    pipe->tile_index = tile_index;
    pipe->stream_type = stream_type;
    pipe->decoder = VideoDecoderFactory::Create(CodecType::UNKNOWN, 0, 0, true);
    pipe->scaler = VideoScalerFactory::Create(0, 0, PixelFormat::NV12, target_w, target_h, PixelFormat::RGBA32);
    pipe->queue = std::make_unique<LiveQueue>(config_.max_queue_depth);
    pipe->active = true;
    pipe->paused = false;

    ChannelPipeline* raw_pipe = pipe.get();
    pipe->sub_id = StreamBroker::Instance().Subscribe(
        channel_id, stream_type,
        [raw_pipe](const MediaPacketPtr& pkt) {
            if (!raw_pipe->active || raw_pipe->paused || !pkt || pkt->IsAudio()) return;
            std::lock_guard<std::mutex> lk(raw_pipe->packet_mutex);
            if (raw_pipe->packet_queue.size() >= raw_pipe->max_packet_depth) {
                raw_pipe->packet_queue.pop_front();
            }
            raw_pipe->packet_queue.push_back(pkt);
            raw_pipe->packet_cv.notify_one();
        }
    );

    pipe->worker_thread = std::thread(&LiveController::ChannelWorkerLoop, this, raw_pipe, target_w, target_h);
    pipelines_[channel_id] = std::move(pipe);
}

void LiveController::ChannelWorkerLoop(ChannelPipeline* pipeline, uint32_t target_w, uint32_t target_h) {
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
                    if (display_backend_) {
                        display_backend_->RenderTile(pipeline->tile_index, pipeline->channel_id, scaled_frame);
                    }
                    pipeline->rendered_count++;

                    // Track FPS
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
        m.width = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstWidth() : (display_backend_ ? display_backend_->GetWidth() : 1920);
        m.height = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstHeight() : (display_backend_ ? display_backend_->GetHeight() : 1080);
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
        m.width = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstWidth() : (display_backend_ ? display_backend_->GetWidth() : 1920);
        m.height = fullscreen_pipeline_->scaler ? fullscreen_pipeline_->scaler->GetDstHeight() : (display_backend_ ? display_backend_->GetHeight() : 1080);
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
