#pragma once

#include "nvr/common/types.h"
#include <vector>
#include <cstdint>
#include <functional>

namespace nvr {

using DepacketizedCallback = std::function<void(const MediaPacketPtr&)>;

class RtpDepacketizer {
public:
    RtpDepacketizer(int channel_id, StreamType stream_type, CodecType codec, DepacketizedCallback callback, uint32_t clock_rate = 90000);
    ~RtpDepacketizer() = default;

    void ProcessRtpPacket(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);

    void Reset();

private:
    void ProcessH264(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);
    void ProcessH265(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit);
    void ProcessAac(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num);
    void ProcessG711(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num);

    void EmitPacket(const std::vector<uint8_t>& data, bool is_keyframe, uint32_t rtp_timestamp, uint16_t seq_num);

    int channel_id_{0};
    StreamType stream_type_{StreamType::MAIN};
    CodecType codec_{CodecType::H264};
    DepacketizedCallback callback_;
    uint32_t clock_rate_{90000};

    // Fragmentation reassembly buffer
    std::vector<uint8_t> fu_buffer_;
    bool fu_in_progress_{false};
    bool fu_is_keyframe_{false};
    uint32_t fu_timestamp_{0};
    uint16_t fu_start_seq_{0};

    uint32_t last_rtp_timestamp_{0};
    int64_t unwrapped_rtp_timestamp_{0};
    int64_t base_pts_us_{0};
    bool has_base_pts_{false};
};

} // namespace nvr
