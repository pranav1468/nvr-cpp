#pragma once

#include "nvr/common/types.h"
#include "nvr/live/live_queue.h"
#include "nvr/live/display_backend.h"
#include "nvr/media/video_decoder.h"
#include "nvr/media/video_scaler.h"
#include "nvr/media/stream_broker.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace nvr {

struct LiveTileMetrics {
    int channel_id{0};
    int tile_index{0};
    StreamType stream_type{StreamType::SUB};
    uint32_t width{0};
    uint32_t height{0};
    uint64_t enqueued_frames{0};
    uint64_t dropped_frames{0};
    uint64_t rendered_frames{0};
    double estimated_fps{0.0};
    bool is_fullscreen{false};
};

class LiveController {
public:
    static LiveController& Instance();

    void Configure(const LiveConfig& config);
    void SetDisplayBackend(DisplayBackendPtr backend);
    DisplayBackendPtr GetDisplayBackend() const;

    // Layout configuration (1, 4, 6, 8 cameras)
    bool SetLayout(LiveGridLayout layout);
    LiveGridLayout GetCurrentLayout() const;
    int GetActiveTileCount() const;

    // Fullscreen single-camera view
    bool SetFullscreen(int channel_id);
    bool ExitFullscreen();
    bool IsFullscreen() const;
    int GetFullscreenChannelId() const;

    // Channel-to-tile assignments
    void AssignChannels(const std::vector<int>& channel_ids);
    std::vector<int> GetAssignedChannels() const;

    // Start / Stop the entire Live View pipeline
    void Start();
    void Stop();

    // Query metrics for UI display and health telemetry
    std::vector<LiveTileMetrics> GetTileMetrics() const;
    LiveTileMetrics GetChannelMetrics(int channel_id) const;

private:
    LiveController();
    ~LiveController();
    LiveController(const LiveController&) = delete;
    LiveController& operator=(const LiveController&) = delete;

    struct ChannelPipeline {
        int channel_id{0};
        int tile_index{0};
        StreamType stream_type{StreamType::SUB};
        VideoDecoderPtr decoder;
        VideoScalerPtr scaler;
        std::unique_ptr<LiveQueue> queue;
        StreamBroker::SubscriptionId sub_id{0};
        std::atomic<bool> active{false};
        std::atomic<bool> paused{false};
        std::thread worker_thread;
        std::atomic<uint64_t> rendered_count{0};
        std::chrono::steady_clock::time_point last_frame_time;
        double current_fps{0.0};
    };

    void SetupGridPipelines();
    void TeardownGridPipelines();
    void StartChannelPipeline(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h);
    void StopChannelPipeline(int channel_id);
    void ChannelWorkerLoop(ChannelPipeline* pipeline, uint32_t target_w, uint32_t target_h);

    mutable std::mutex mutex_;
    LiveConfig config_;
    DisplayBackendPtr display_backend_;

    LiveGridLayout current_layout_{LiveGridLayout::GRID_4};
    LiveGridLayout previous_layout_{LiveGridLayout::GRID_4};
    int fullscreen_channel_id_{-1};
    bool was_main_created_for_fullscreen_{false};

    std::vector<int> assigned_channels_;
    std::unordered_map<int, std::unique_ptr<ChannelPipeline>> pipelines_;
    std::unique_ptr<ChannelPipeline> fullscreen_pipeline_;

    std::atomic<bool> running_{false};
};

} // namespace nvr
