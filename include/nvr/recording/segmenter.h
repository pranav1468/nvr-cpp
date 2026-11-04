#pragma once

#include "nvr/common/types.h"
#include "nvr/recording/atomic_writer.h"
#include "nvr/media/video_decoder.h"
#include <string>
#include <memory>
#include <mutex>
#include <vector>

namespace nvr {

class Segmenter {
public:
    Segmenter(int channel_id, const std::string& output_directory, int segment_duration_seconds = 60);
    ~Segmenter();

    void PushPacket(const MediaPacketPtr& packet);

    void FlushAndStop();

    void ConfigureAudio(CodecType codec, uint32_t sample_rate = 8000, uint8_t channels = 1);

    void SetOutputDirectory(const std::string& new_dir);

    bool IsRecording() const;
    uint32_t GetCurrentFrameCount() const;
    uint64_t GetCurrentBytesWritten() const;

private:
    void RotateSegment(const MediaPacketPtr& next_keyframe_packet);
    void ExtractAndCacheParameterSets(const MediaPacketPtr& packet);
    void InjectCachedParameterSets();

    int channel_id_{0};
    std::string output_dir_;
    int segment_duration_ms_{60000};
    AtomicWriter writer_;
    mutable std::mutex mutex_;
    bool is_recording_{false};
    bool waiting_for_first_keyframe_{true};

    // Cached parameter sets for H.264 / H.265
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;
    std::vector<uint8_t> cached_vps_;
    CodecType cached_codec_{CodecType::UNKNOWN};
    uint32_t cached_width_{0};
    uint32_t cached_height_{0};
    bool has_audio_configured_{false};

    // Dedicated keyframe decoder for background companion snapshots
    VideoDecoderPtr thumbnail_decoder_;
    DecodedFramePtr latest_segment_thumbnail_;
};

} // namespace nvr
