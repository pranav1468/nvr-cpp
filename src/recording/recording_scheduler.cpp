#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/logger.h"

namespace nvr {

RecordingScheduler& RecordingScheduler::Instance() {
    static RecordingScheduler instance;
    return instance;
}

RecordingScheduler::~RecordingScheduler() {
    StopAll();
}

void RecordingScheduler::Configure(const StorageConfig& storage_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_config_ = storage_config;
}

bool RecordingScheduler::StartChannelRecording(int channel_id, RecordMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (channels_.find(channel_id) != channels_.end()) {
        LOG_WARN << "[Channel " << channel_id << "] Recording already active";
        return true;
    }

    if (mode == RecordMode::DISABLED) {
        LOG_INFO << "[Channel " << channel_id << "] Recording mode is DISABLED";
        return true;
    }

    auto segmenter = std::make_shared<Segmenter>(
        channel_id, 
        storage_config_.recording_path, 
        storage_config_.segment_duration_seconds
    );

    std::weak_ptr<Segmenter> weak_seg = segmenter;

    // Subscribe to compressed MAIN stream packets from the StreamBroker
    auto sub_id = StreamBroker::Instance().Subscribe(
        channel_id, 
        StreamType::MAIN,
        [weak_seg](const MediaPacketPtr& packet) {
            if (auto seg = weak_seg.lock()) {
                seg->PushPacket(packet);
            }
        }
    );

    channels_[channel_id] = ChannelRecordState{
        channel_id,
        mode,
        std::move(segmenter),
        sub_id
    };

    LOG_INFO << "[Channel " << channel_id << "] Started recording scheduler (Mode: " 
             << static_cast<int>(mode) << ", Target: " << storage_config_.recording_path << ")";
    return true;
}

void RecordingScheduler::StopChannelRecording(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        StreamBroker::Instance().Unsubscribe(it->second.subscription_id);
        if (it->second.segmenter) {
            it->second.segmenter->FlushAndStop();
        }
        channels_.erase(it);
        LOG_INFO << "[Channel " << channel_id << "] Stopped recording";
    }
}

void RecordingScheduler::StopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [ch_id, state] : channels_) {
        StreamBroker::Instance().Unsubscribe(state.subscription_id);
        if (state.segmenter) {
            state.segmenter->FlushAndStop();
        }
    }
    channels_.clear();
    LOG_INFO << "Stopped all active recording channels";
}

bool RecordingScheduler::IsChannelRecording(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end() && it->second.segmenter && it->second.segmenter->IsRecording());
}

uint32_t RecordingScheduler::GetChannelFrameCount(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end() && it->second.segmenter) {
        return it->second.segmenter->GetCurrentFrameCount();
    }
    return 0;
}

uint64_t RecordingScheduler::GetChannelBytesWritten(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end() && it->second.segmenter) {
        return it->second.segmenter->GetCurrentBytesWritten();
    }
    return 0;
}

} // namespace nvr
