#pragma once

#include "nvr/common/types.h"
#include "nvr/live/live_queue.h"
#include "nvr/live/display_backend.h"
#include "nvr/media/video_decoder.h"
#include "nvr/media/stream_broker.h"
#include <memory>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

namespace nvr {

enum class TileConnectionState {
    OFFLINE = 0,
    CONNECTING = 1,
    STREAMING = 2,
    STALLED = 3
};

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
    TileConnectionState state{TileConnectionState::OFFLINE};
};

struct TileConfig {
    int tile_index{0};
    int channel_id{0};
    StreamType stream_type{StreamType::SUB};
    uint32_t target_w{0};
    uint32_t target_h{0};
};

class LiveController {
public:
    static LiveController& Instance();

    void Configure(const LiveConfig& config);
    void SetDisplayBackend(DisplayBackendPtr backend);
    DisplayBackendPtr GetDisplayBackend() const;

    // Preset Layouts (1, 4, 6, 8 cameras)
    bool SetLayout(LiveGridLayout layout);
    LiveGridLayout GetCurrentLayout() const;
    int GetActiveTileCount() const;

    // Fully Dynamic Frontend-Driven Layouts
    bool SetCustomGrid(int cols, int rows, const std::vector<int>& channel_ids = {});
    bool SetTileLayout(const std::vector<TileConfig>& tiles);

    // On-Demand Channel Feed Activation/Deactivation
    bool ActivateChannel(int channel_id, int tile_index = -1, StreamType stream_type = StreamType::SUB, uint32_t target_w = 0, uint32_t target_h = 0);
    bool DeactivateChannel(int channel_id);
    bool SetActiveChannels(const std::vector<int>& channel_ids, StreamType stream_type = StreamType::SUB);

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

    struct TileTarget {
        int tile_index{0};
    };

    struct ChannelPipeline {
        int channel_id{0};
        StreamType stream_type{StreamType::SUB};
        VideoDecoderPtr decoder;
        mutable std::mutex targets_mutex;
        std::vector<TileTarget> targets;
        std::unique_ptr<LiveQueue> queue;
        StreamBroker::SubscriptionId sub_id{0};
        std::atomic<bool> active{false};
        std::atomic<bool> paused{false};
        std::mutex packet_mutex;
        std::condition_variable packet_cv;
        std::deque<MediaPacketPtr> packet_queue;
        const size_t max_packet_depth{12};
        std::thread worker_thread;
        std::atomic<uint64_t> rendered_count{0};
        std::atomic<int64_t> last_frame_time_ms{0};
        double current_fps{0.0};
        std::atomic<bool> first_frame_rendered{false};
        std::mutex callback_mutex;
        std::function<void()> on_first_frame;
        std::atomic<uint32_t> stream_width{0};
        std::atomic<uint32_t> stream_height{0};
        std::atomic<bool> awaiting_keyframe{true};
        mutable std::mutex frame_mutex;
        DecodedFramePtr last_rendered_frame;
    };

    void SetupGridPipelines();
    void ReconcilePipelines(const std::vector<TileConfig>& desired_tiles);
    void TeardownPipeline(std::shared_ptr<ChannelPipeline> pipe);
    void TeardownPipelines(std::vector<std::shared_ptr<ChannelPipeline>>& pipes);
    // Precondition: Caller must hold layout_mutex_
    void StartChannelPipeline(int channel_id, int tile_index, StreamType stream_type, uint32_t target_w, uint32_t target_h);
    void StartChannelPipelineMulti(int channel_id, StreamType stream_type, const std::vector<TileConfig>& targets);
    // Precondition: Caller must hold layout_mutex_
    void StopChannelPipeline(int channel_id);
    void ChannelWorkerLoop(std::shared_ptr<ChannelPipeline> pipeline);

    mutable std::mutex mutex_;
    mutable std::mutex backend_mutex_;
    std::mutex layout_mutex_;
    LiveConfig config_;
    DisplayBackendPtr display_backend_;

    LiveGridLayout current_layout_{LiveGridLayout::GRID_4};
    LiveGridLayout previous_layout_{LiveGridLayout::GRID_4};
    int fullscreen_channel_id_{-1};

    std::vector<int> assigned_channels_;
    int custom_cols_{0};
    int custom_rows_{0};
    std::vector<TileConfig> custom_tiles_;
    std::unordered_map<int, std::shared_ptr<ChannelPipeline>> pipelines_;
    std::shared_ptr<ChannelPipeline> fullscreen_pipeline_;

    std::atomic<bool> running_{false};
};

} // namespace nvr
