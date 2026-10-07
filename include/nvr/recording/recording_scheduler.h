#pragma once

#include "nvr/common/types.h"
#include "nvr/recording/segmenter.h"
#include "nvr/media/stream_broker.h"
#include <unordered_map>
#include <memory>
#include <mutex>

namespace nvr {

class RecordingScheduler {
public:
    static RecordingScheduler& Instance();

    void Configure(const StorageConfig& storage_config);

    bool StartChannelRecording(int channel_id, RecordMode mode = RecordMode::CONTINUOUS);

    void StopChannelRecording(int channel_id);

    void StopAll();

    bool IsChannelRecording(int channel_id) const;

    uint32_t GetChannelFrameCount(int channel_id) const;
    uint64_t GetChannelBytesWritten(int channel_id) const;

private:
    RecordingScheduler() = default;
    ~RecordingScheduler();
    RecordingScheduler(const RecordingScheduler&) = delete;
    RecordingScheduler& operator=(const RecordingScheduler&) = delete;

    struct ChannelRecordState {
        int channel_id;
        RecordMode mode;
        std::unique_ptr<Segmenter> segmenter;
        StreamBroker::SubscriptionId subscription_id;
    };

    StorageConfig storage_config_;
    mutable std::mutex mutex_;
    std::unordered_map<int, ChannelRecordState> channels_;
};

} // namespace nvr
