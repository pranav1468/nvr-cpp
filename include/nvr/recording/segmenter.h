#pragma once

#include "nvr/common/types.h"
#include "nvr/recording/atomic_writer.h"
#include <string>
#include <memory>
#include <mutex>

namespace nvr {

class Segmenter {
public:
    Segmenter(int channel_id, const std::string& output_directory, int segment_duration_seconds = 60);
    ~Segmenter();

    void PushPacket(const MediaPacketPtr& packet);

    void FlushAndStop();

    void ConfigureAudio(CodecType codec, uint32_t sample_rate = 8000, uint8_t channels = 1);

    bool IsRecording() const;
    uint32_t GetCurrentFrameCount() const;
    uint64_t GetCurrentBytesWritten() const;

private:
    void RotateSegment(const MediaPacketPtr& next_keyframe_packet);

    int channel_id_{0};
    std::string output_dir_;
    int segment_duration_ms_{60000};
    AtomicWriter writer_;
    mutable std::mutex mutex_;
    bool is_recording_{false};
    bool waiting_for_first_keyframe_{true};
};

} // namespace nvr
