#pragma once

#include "nvr/common/types.h"
#include <vector>
#include <map>
#include <cstdint>
#include <functional>

#include <chrono>

namespace nvr {

using DepacketizedCallback = std::function<void(const MediaPacketPtr&)>;

class RtpDepacketizer {
public:
    RtpDepacketizer(int channel_id, StreamType stream_type, CodecType codec, DepacketizedCallback callback, uint32_t clock_rate = 90000);
    ~RtpDepacketizer() = default;

    void ProcessRtpPacket(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);

    void Flush();
    void Reset();

private:
    struct QueuedRtpPacket {
        uint16_t seq_num{0};
        uint32_t timestamp{0};
        bool marker{false};
        std::chrono::steady_clock::time_point arrival_time{};
        std::vector<uint8_t> payload;
    };

    struct SeqNumLess {
        bool operator()(uint16_t a, uint16_t b) const {
            return static_cast<int16_t>(a - b) < 0;
        }
    };

    void DrainReorderBuffer();
    void ProcessVideoPacketInternal(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);
    void ProcessH264(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);
    void ProcessH265(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);
    void ProcessAac(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num);
    void ProcessG711(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num);

    void AppendNalToAu(const uint8_t* nal_data, size_t nal_len, bool is_key, uint32_t rtp_timestamp, uint16_t seq_num);
    void FlushAu();
    void EmitPacket(const std::vector<uint8_t>& data, bool is_keyframe, uint32_t rtp_timestamp, uint16_t seq_num);

    int channel_id_{0};
    StreamType stream_type_{StreamType::MAIN};
    CodecType codec_{CodecType::H264};
    DepacketizedCallback callback_;
    uint32_t clock_rate_{90000};

    // Sequence reordering / jitter buffer
    bool has_seq_{false};
    uint16_t expected_seq_{0};
    std::map<uint16_t, QueuedRtpPacket, SeqNumLess> reorder_buffer_;

    // Access Unit (AU) assembly buffer
    std::vector<uint8_t> au_buffer_;
    bool au_has_data_{false};
    bool au_is_keyframe_{false};
    uint32_t au_timestamp_{0};
    uint16_t au_seq_num_{0};

    // Fragmentation reassembly buffer
    std::vector<uint8_t> fu_buffer_;
    bool fu_in_progress_{false};
    bool fu_is_keyframe_{false};
    uint32_t fu_timestamp_{0};
    uint16_t fu_start_seq_{0};
    uint16_t fu_last_seq_{0};

    uint32_t last_rtp_timestamp_{0};
    int64_t unwrapped_rtp_timestamp_{0};
    int64_t base_pts_us_{0};
    bool has_base_pts_{false};
};

} // namespace nvr
