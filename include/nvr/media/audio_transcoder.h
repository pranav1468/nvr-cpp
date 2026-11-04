#pragma once

#include "nvr/common/types.h"
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <cstdint>

namespace nvr {

class AudioTranscoder {
public:
    using PacketCallback = std::function<void(const MediaPacketPtr&)>;

    AudioTranscoder(int channel_id, StreamType stream_type, PacketCallback output_callback);
    ~AudioTranscoder();

    // Reset internal state and buffers
    void Reset();

    // Process incoming audio packet (PCMA, PCMU, or AAC)
    void ProcessPacket(const MediaPacketPtr& packet);

    // Flush any pending buffered audio
    void Flush();

private:
    bool InitializeEncoder(uint32_t sample_rate, uint8_t channels);
    void CloseEncoder();
    void EncodeFrame(const float* samples, size_t count);

    int channel_id_{0};
    StreamType stream_type_{StreamType::MAIN};
    PacketCallback output_callback_;

    std::mutex mutex_;
    bool encoder_initialized_{false};
    void* avcodec_lib_{nullptr};
    void* avutil_lib_{nullptr};
    void* codec_ctx_{nullptr};
    void* av_frame_{nullptr};
    void* av_pkt_{nullptr};

    uint32_t sample_rate_{8000};
    uint8_t channels_{1};
    int64_t pts_us_{0};
    bool has_pts_{false};
    uint64_t total_samples_encoded_{0};

    std::vector<float> pcm_fifo_;
};

} // namespace nvr

