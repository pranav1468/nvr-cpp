#pragma once

#include "nvr/common/types.h"
#include "nvr/recording/segmenter.h"
#include "nvr/media/stream_broker.h"
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include "3rdparty/nlohmann/json.hpp"

namespace nvr {

class RecordingScheduler {
public:
    static RecordingScheduler& Instance();

    void Configure(const StorageConfig& storage_config);
    void SetStoragePath(const std::string& new_path);
    std::string GetStoragePath() const;

    bool StartChannelRecording(int channel_id, RecordMode mode = RecordMode::CONTINUOUS);

    void StopChannelRecording(int channel_id);

    void StopAll();

    bool IsChannelRecording(int channel_id) const;
    bool IsChannelActive(int channel_id) const;

    uint32_t GetChannelFrameCount(int channel_id) const;
    uint64_t GetChannelBytesWritten(int channel_id) const;
    uint64_t GetChannelDroppedPackets(int channel_id) const;
    uint64_t GetChannelPrerollPacketCount(int channel_id) const;
    uint64_t GetChannelPrerollBytes(int channel_id) const;

    // Motion event control for RecordMode::MOTION_ONLY
    void SetMotionEvent(int channel_id, bool motion_active);
    void SetPostRollSeconds(int channel_id, int seconds);
    void SetPreRollSeconds(int channel_id, int seconds);

    // Event control for RecordMode::EVENT_ONLY and SCHEDULED event slots
    void SetEventActive(int channel_id, const std::string& event_type, bool active);
    void TriggerEventRecording(int channel_id, const std::string& event_type, int pre_roll_sec = 5, int post_roll_sec = 10);
    bool IsEventActive(int channel_id) const;
    std::string GetCurrentEventType(int channel_id) const;


    // Enqueue packet asynchronously from StreamBroker ingress callback
    void EnqueuePacket(int channel_id, const MediaPacketPtr& packet);

    // Flush and synchronize channel worker queue
    void FlushChannel(int channel_id);

    // Schedule control & query
    nlohmann::json GetChannelSchedule(int channel_id) const;
    bool SetChannelSchedule(int channel_id, const nlohmann::json& schedule_payload);

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
        std::atomic<bool> has_motion_occurred{false};
        std::chrono::steady_clock::time_point last_motion_time;
        int post_roll_seconds{5};
        int preroll_seconds{5};
        bool is_in_motion_recording{false};

        // Event mode state machine
        std::atomic<bool> event_active{false};
        std::atomic<bool> has_event_occurred{false};
        std::chrono::steady_clock::time_point last_event_time;
        std::string current_event_type;
        mutable std::mutex event_mutex;
        bool is_in_event_recording{false};

        // In-memory rolling pre-roll buffer (used as fallback or worker staging)
        std::mutex preroll_mutex;
        std::deque<MediaPacketPtr> preroll_buffer;
        size_t preroll_bytes{0};
        const size_t max_preroll_bytes{8 * 1024 * 1024}; // 8 MB bound
        const size_t max_preroll_packets{150};


    };

    void ChannelWorkerLoop(std::shared_ptr<ChannelRecordState> state);

    StorageConfig storage_config_;
    mutable std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<ChannelRecordState>> channels_;

    std::unordered_map<int, std::pair<bool, std::string>> channel_event_states_;
    std::unordered_map<int, nlohmann::json> channel_schedules_;
};

} // namespace nvr
