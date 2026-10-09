#pragma once

#include "nvr/common/types.h"
#include "nvr/recording/segmenter.h"
#include "nvr/media/stream_broker.h"
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace nvr {

struct ScheduleTimeWindow {
    int start_hour{0};       // 0..23
    int start_minute{0};     // 0..59
    int end_hour{24};        // 0..24
    int end_minute{0};       // 0..59
    uint8_t days_mask{0x7F}; // Bit 0=Sun, 1=Mon, ..., 6=Sat (0x7F = all 7 days)
};

class RecordingScheduler {
public:
    static RecordingScheduler& Instance();

    void Configure(const StorageConfig& storage_config);

    bool StartChannelRecording(int channel_id, RecordMode mode = RecordMode::CONTINUOUS);

    void StopChannelRecording(int channel_id);

    void StopAll();

    bool IsChannelRecording(int channel_id) const;
    bool IsChannelActive(int channel_id) const;

    uint32_t GetChannelFrameCount(int channel_id) const;
    uint64_t GetChannelBytesWritten(int channel_id) const;
    uint64_t GetChannelDroppedPackets(int channel_id) const;

    // Motion event control for RecordMode::MOTION_ONLY
    void SetMotionEvent(int channel_id, bool motion_active);

    // Schedule configuration for RecordMode::SCHEDULED
    void SetSchedule(int channel_id, const std::vector<ScheduleTimeWindow>& schedule);
    bool IsScheduleActiveNow(int channel_id, int64_t now_ms) const;

    // Enqueue packet asynchronously from StreamBroker ingress callback
    void EnqueuePacket(int channel_id, const MediaPacketPtr& packet);

    // Flush and synchronize channel worker queue
    void FlushChannel(int channel_id);

private:
    RecordingScheduler() = default;
    ~RecordingScheduler();
    RecordingScheduler(const RecordingScheduler&) = delete;
    RecordingScheduler& operator=(const RecordingScheduler&) = delete;

    struct ChannelRecordState {
        int channel_id{0};
        RecordMode mode{RecordMode::CONTINUOUS};
        std::shared_ptr<Segmenter> segmenter;
        StreamBroker::SubscriptionId subscription_id{0};

        std::thread worker_thread;
        std::atomic<bool> running{false};
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::deque<MediaPacketPtr> input_queue;
        std::atomic<uint64_t> dropped_packets_count{0};
        std::atomic<bool> needs_keyframe_resync{false};

        // Motion mode state machine
        std::atomic<bool> motion_active{false};
        std::chrono::steady_clock::time_point last_motion_time;
        int post_roll_seconds{5};
        bool is_in_motion_recording{false};
        std::deque<MediaPacketPtr> preroll_buffer;
        const size_t max_preroll_packets{120};

        // Scheduled mode configuration
        std::vector<ScheduleTimeWindow> schedule;
    };

    void ChannelWorkerLoop(std::shared_ptr<ChannelRecordState> state);

    StorageConfig storage_config_;
    mutable std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<ChannelRecordState>> channels_;
};

} // namespace nvr
