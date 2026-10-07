#include "nvr/ingress/rtp_depacketizer.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/logger.h"

namespace nvr {

RtpDepacketizer::RtpDepacketizer(int channel_id, StreamType stream_type, CodecType codec, DepacketizedCallback callback)
    : channel_id_(channel_id), stream_type_(stream_type), codec_(codec), callback_(std::move(callback)) {
    fu_buffer_.reserve(256 * 1024);
}

void RtpDepacketizer::Reset() {
    fu_buffer_.clear();
    fu_in_progress_ = false;
    fu_is_keyframe_ = false;
    has_base_pts_ = false;
    unwrapped_rtp_timestamp_ = 0;
}

void RtpDepacketizer::ProcessRtpPacket(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit) {
    if (!payload || size == 0) return;

    if (codec_ == CodecType::H264) {
        ProcessH264(payload, size, rtp_timestamp, seq_num, marker_bit);
    } else if (codec_ == CodecType::H265) {
        ProcessH265(payload, size, rtp_timestamp, seq_num, marker_bit);
    }
}

void RtpDepacketizer::ProcessH264(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool /*marker_bit*/) {
    if (size < 1) return;

    uint8_t nal_header = payload[0];
    uint8_t nal_type = nal_header & 0x1F;

    // Single NAL unit packet (1..23)
    if (nal_type >= 1 && nal_type <= 23) {
        std::vector<uint8_t> frame;
        frame.reserve(4 + size);
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(0x01);
        frame.insert(frame.end(), payload, payload + size);

        bool is_key = (nal_type == 5); // IDR slice
        EmitPacket(frame, is_key, rtp_timestamp, seq_num);
        return;
    }

    // STAP-A aggregation packet (type 24)
    if (nal_type == 24) {
        size_t offset = 1;
        while (offset + 2 <= size) {
            uint16_t nalu_size = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nalu_size > size) break;

            if (nalu_size > 0) {
                uint8_t sub_type = payload[offset] & 0x1F;
                bool is_key = (sub_type == 5);

                std::vector<uint8_t> frame;
                frame.reserve(4 + nalu_size);
                frame.push_back(0x00);
                frame.push_back(0x00);
                frame.push_back(0x00);
                frame.push_back(0x01);
                frame.insert(frame.end(), payload + offset, payload + offset + nalu_size);

                EmitPacket(frame, is_key, rtp_timestamp, seq_num);
            }
            offset += nalu_size;
        }
        return;
    }

    // FU-A Fragmentation Unit (type 28)
    if (nal_type == 28) {
        if (size < 2) return;
        uint8_t fu_indicator = payload[0];
        uint8_t fu_header = payload[1];
        bool start_bit = (fu_header & 0x80) != 0;
        bool end_bit = (fu_header & 0x40) != 0;
        uint8_t original_nal_type = fu_header & 0x1F;

        if (start_bit) {
            fu_buffer_.clear();
            // Prefix 4-byte Annex-B start code
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x01);

            // Reconstruct original NAL header
            uint8_t reconstructed_hdr = (fu_indicator & 0xE0) | original_nal_type;
            fu_buffer_.push_back(reconstructed_hdr);

            fu_buffer_.insert(fu_buffer_.end(), payload + 2, payload + size);
            fu_in_progress_ = true;
            fu_is_keyframe_ = (original_nal_type == 5);
            fu_timestamp_ = rtp_timestamp;
            fu_start_seq_ = seq_num;
        } else if (fu_in_progress_) {
            fu_buffer_.insert(fu_buffer_.end(), payload + 2, payload + size);
        }

        if (end_bit && fu_in_progress_) {
            EmitPacket(fu_buffer_, fu_is_keyframe_, fu_timestamp_, seq_num);
            fu_buffer_.clear();
            fu_in_progress_ = false;
        }
    }
}

void RtpDepacketizer::ProcessH265(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool /*marker_bit*/) {
    if (size < 2) return;

    uint8_t nal_type = (payload[0] >> 1) & 0x3F;

    // Single NAL (types 0..47)
    if (nal_type <= 47) {
        std::vector<uint8_t> frame;
        frame.reserve(4 + size);
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(0x01);
        frame.insert(frame.end(), payload, payload + size);

        bool is_key = (nal_type >= 16 && nal_type <= 21); // IDR / CRA / BLA
        EmitPacket(frame, is_key, rtp_timestamp, seq_num);
        return;
    }

    // FU Fragmentation Unit (type 49)
    if (nal_type == 49) {
        if (size < 3) return;
        uint8_t fu_header = payload[2];
        bool start_bit = (fu_header & 0x80) != 0;
        bool end_bit = (fu_header & 0x40) != 0;
        uint8_t original_nal_type = fu_header & 0x3F;

        if (start_bit) {
            fu_buffer_.clear();
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x00);
            fu_buffer_.push_back(0x01);

            // Reconstruct 2-byte H.265 NAL header
            uint8_t hdr1 = (payload[0] & 0x81) | (original_nal_type << 1);
            uint8_t hdr2 = payload[1];
            fu_buffer_.push_back(hdr1);
            fu_buffer_.push_back(hdr2);

            fu_buffer_.insert(fu_buffer_.end(), payload + 3, payload + size);
            fu_in_progress_ = true;
            fu_is_keyframe_ = (original_nal_type >= 16 && original_nal_type <= 21);
            fu_timestamp_ = rtp_timestamp;
            fu_start_seq_ = seq_num;
        } else if (fu_in_progress_) {
            fu_buffer_.insert(fu_buffer_.end(), payload + 3, payload + size);
        }

        if (end_bit && fu_in_progress_) {
            EmitPacket(fu_buffer_, fu_is_keyframe_, fu_timestamp_, seq_num);
            fu_buffer_.clear();
            fu_in_progress_ = false;
        }
    }
}

void RtpDepacketizer::EmitPacket(const std::vector<uint8_t>& data, bool is_keyframe, uint32_t rtp_timestamp, uint16_t seq_num) {
    if (!callback_ || data.empty()) return;

    auto packet = std::make_shared<MediaPacket>();
    packet->channel_id = channel_id_;
    packet->stream_type = stream_type_;
    packet->codec = codec_;
    packet->is_keyframe = is_keyframe;
    packet->rtp_timestamp = rtp_timestamp;
    packet->sequence_number = seq_num;
    packet->wall_time_ms = time_utils::WallTimeMs();

    // Convert RTP 90kHz timestamp to microseconds with seamless 32-bit wrap unrolling
    if (!has_base_pts_) {
        base_pts_us_ = time_utils::MonotonicUs();
        last_rtp_timestamp_ = rtp_timestamp;
        unwrapped_rtp_timestamp_ = 0;
        has_base_pts_ = true;
        packet->pts_us = base_pts_us_;
    } else {
        // Signed 32-bit difference handles wrap around from 0xFFFFFFFF -> 0 smoothly
        int32_t delta_ticks = static_cast<int32_t>(rtp_timestamp - last_rtp_timestamp_);
        if (delta_ticks < -900000 || delta_ticks > 900000) {
            // Clock jump >10s detected; re-anchor to monotonic clock
            base_pts_us_ = time_utils::MonotonicUs();
            unwrapped_rtp_timestamp_ = 0;
        } else {
            unwrapped_rtp_timestamp_ += delta_ticks;
        }
        last_rtp_timestamp_ = rtp_timestamp;

        int64_t delta_us = (unwrapped_rtp_timestamp_ * 1000000LL) / 90000LL;
        packet->pts_us = base_pts_us_ + delta_us;
    }
    packet->dts_us = packet->pts_us;
    packet->data = data;

    callback_(packet);
}

} // namespace nvr
