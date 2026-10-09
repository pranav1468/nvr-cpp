#include "nvr/ingress/rtp_depacketizer.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/logger.h"

namespace nvr {

RtpDepacketizer::RtpDepacketizer(int channel_id, StreamType stream_type, CodecType codec, DepacketizedCallback callback, uint32_t clock_rate)
    : channel_id_(channel_id), stream_type_(stream_type), codec_(codec), callback_(std::move(callback)), clock_rate_(clock_rate) {
    fu_buffer_.reserve(256 * 1024);
}

void RtpDepacketizer::Reset() {
    FlushAu();
    fu_buffer_.clear();
    fu_in_progress_ = false;
    fu_is_keyframe_ = false;
    fu_start_seq_ = 0;
    fu_last_seq_ = 0;
    has_seq_ = false;
    expected_seq_ = 0;
    reorder_buffer_.clear();
    has_base_pts_ = false;
    unwrapped_rtp_timestamp_ = 0;
}

void RtpDepacketizer::Flush() {
    FlushAu();
}

void RtpDepacketizer::ProcessRtpPacket(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit) {
    if (!payload || size == 0) return;

    if (codec_ == CodecType::AAC) {
        ProcessAac(payload, size, rtp_timestamp, seq_num);
        return;
    } else if (codec_ == CodecType::PCMA || codec_ == CodecType::PCMU) {
        ProcessG711(payload, size, rtp_timestamp, seq_num);
        return;
    }

    // Video streams (H.264 / H.265): pass through sequence reordering buffer
    if (!has_seq_) {
        has_seq_ = true;
        expected_seq_ = seq_num;
    }

    if (seq_num == expected_seq_) {
        ProcessVideoPacketInternal(payload, size, rtp_timestamp, seq_num, marker_bit);
        expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);

        while (!reorder_buffer_.empty()) {
            auto it = reorder_buffer_.find(expected_seq_);
            if (it != reorder_buffer_.end()) {
                QueuedRtpPacket qp = std::move(it->second);
                reorder_buffer_.erase(it);
                ProcessVideoPacketInternal(qp.payload.data(), qp.payload.size(), qp.timestamp, qp.seq_num, qp.marker);
                expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
            } else {
                break;
            }
        }
    } else {
        int16_t diff = static_cast<int16_t>(seq_num - expected_seq_);
        if (diff > 0) {
            QueuedRtpPacket qp;
            qp.seq_num = seq_num;
            qp.timestamp = rtp_timestamp;
            qp.marker = marker_bit;
            qp.payload.assign(payload, payload + size);
            reorder_buffer_[seq_num] = std::move(qp);

            if (reorder_buffer_.size() >= 16) {
                auto lowest_it = reorder_buffer_.begin();
                expected_seq_ = lowest_it->first;
                QueuedRtpPacket qp_low = std::move(lowest_it->second);
                reorder_buffer_.erase(lowest_it);
                ProcessVideoPacketInternal(qp_low.payload.data(), qp_low.payload.size(), qp_low.timestamp, qp_low.seq_num, qp_low.marker);
                expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
            }
        }
    }
}

void RtpDepacketizer::ProcessVideoPacketInternal(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit) {
    if (codec_ == CodecType::H264) {
        ProcessH264(payload, size, rtp_timestamp, seq_num, marker_bit);
    } else if (codec_ == CodecType::H265) {
        ProcessH265(payload, size, rtp_timestamp, seq_num, marker_bit);
    }
}

void RtpDepacketizer::AppendNalToAu(const uint8_t* nal_data, size_t nal_len, bool is_key, uint32_t rtp_timestamp, uint16_t seq_num) {
    if (!nal_data || nal_len == 0) return;

    if (au_has_data_ && rtp_timestamp != au_timestamp_) {
        FlushAu();
    }

    if (!au_has_data_) {
        au_buffer_.clear();
        au_timestamp_ = rtp_timestamp;
        au_seq_num_ = seq_num;
        au_is_keyframe_ = false;
        au_has_data_ = true;
    }

    // Prepend 4-byte Annex-B start code
    au_buffer_.push_back(0x00);
    au_buffer_.push_back(0x00);
    au_buffer_.push_back(0x00);
    au_buffer_.push_back(0x01);
    au_buffer_.insert(au_buffer_.end(), nal_data, nal_data + nal_len);

    if (is_key) {
        au_is_keyframe_ = true;
    }
    au_seq_num_ = seq_num;
}

void RtpDepacketizer::FlushAu() {
    if (!au_has_data_ || au_buffer_.empty()) {
        au_has_data_ = false;
        return;
    }
    EmitPacket(au_buffer_, au_is_keyframe_, au_timestamp_, au_seq_num_);
    au_buffer_.clear();
    au_has_data_ = false;
    au_is_keyframe_ = false;
}

void RtpDepacketizer::ProcessH264(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit) {
    if (size < 1) return;

    uint8_t nal_header = payload[0];
    uint8_t nal_type = nal_header & 0x1F;

    // Single NAL unit packet (1..23)
    if (nal_type >= 1 && nal_type <= 23) {
        bool is_key = (nal_type == 5);
        AppendNalToAu(payload, size, is_key, rtp_timestamp, seq_num);
        if (marker_bit) {
            FlushAu();
        }
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
                AppendNalToAu(payload + offset, nalu_size, is_key, rtp_timestamp, seq_num);
            }
            offset += nalu_size;
        }
        if (marker_bit) {
            FlushAu();
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
            uint8_t reconstructed_hdr = (fu_indicator & 0xE0) | original_nal_type;
            fu_buffer_.push_back(reconstructed_hdr);
            fu_buffer_.insert(fu_buffer_.end(), payload + 2, payload + size);
            fu_in_progress_ = true;
            fu_is_keyframe_ = (original_nal_type == 5);
            fu_timestamp_ = rtp_timestamp;
            fu_start_seq_ = seq_num;
            fu_last_seq_ = seq_num;
        } else if (fu_in_progress_) {
            uint16_t expected_seq = static_cast<uint16_t>(fu_last_seq_ + 1);
            if (seq_num != expected_seq) {
                fu_buffer_.clear();
                fu_in_progress_ = false;
                return;
            }
            fu_last_seq_ = seq_num;
            fu_buffer_.insert(fu_buffer_.end(), payload + 2, payload + size);
        }

        if (end_bit && fu_in_progress_) {
            AppendNalToAu(fu_buffer_.data(), fu_buffer_.size(), fu_is_keyframe_, fu_timestamp_, seq_num);
            fu_buffer_.clear();
            fu_in_progress_ = false;
            if (marker_bit) {
                FlushAu();
            }
        }
    }
}

void RtpDepacketizer::ProcessH265(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num, bool marker_bit) {
    if (size < 2) return;

    uint8_t nal_type = (payload[0] >> 1) & 0x3F;

    // Single NAL (types 0..47)
    if (nal_type <= 47) {
        bool is_key = (nal_type >= 16 && nal_type <= 21);
        AppendNalToAu(payload, size, is_key, rtp_timestamp, seq_num);
        if (marker_bit) {
            FlushAu();
        }
        return;
    }

    // AP Aggregation Packet (type 48)
    if (nal_type == 48) {
        size_t offset = 2;
        while (offset + 2 <= size) {
            uint16_t nalu_size = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nalu_size > size) break;

            if (nalu_size > 0) {
                uint8_t sub_type = (payload[offset] >> 1) & 0x3F;
                bool is_key = (sub_type >= 16 && sub_type <= 21);
                AppendNalToAu(payload + offset, nalu_size, is_key, rtp_timestamp, seq_num);
            }
            offset += nalu_size;
        }
        if (marker_bit) {
            FlushAu();
        }
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
            uint8_t hdr1 = (payload[0] & 0x81) | (original_nal_type << 1);
            uint8_t hdr2 = payload[1];
            fu_buffer_.push_back(hdr1);
            fu_buffer_.push_back(hdr2);
            fu_buffer_.insert(fu_buffer_.end(), payload + 3, payload + size);
            fu_in_progress_ = true;
            fu_is_keyframe_ = (original_nal_type >= 16 && original_nal_type <= 21);
            fu_timestamp_ = rtp_timestamp;
            fu_start_seq_ = seq_num;
            fu_last_seq_ = seq_num;
        } else if (fu_in_progress_) {
            uint16_t expected_seq = static_cast<uint16_t>(fu_last_seq_ + 1);
            if (seq_num != expected_seq) {
                fu_buffer_.clear();
                fu_in_progress_ = false;
                return;
            }
            fu_last_seq_ = seq_num;
            fu_buffer_.insert(fu_buffer_.end(), payload + 3, payload + size);
        }

        if (end_bit && fu_in_progress_) {
            AppendNalToAu(fu_buffer_.data(), fu_buffer_.size(), fu_is_keyframe_, fu_timestamp_, seq_num);
            fu_buffer_.clear();
            fu_in_progress_ = false;
            if (marker_bit) {
                FlushAu();
            }
        }
    }
}

void RtpDepacketizer::ProcessAac(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num) {
    if (size < 4) return;

    // RFC 3640 AU-header parsing
    uint16_t au_headers_len_bits = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
    size_t au_headers_len_bytes = (au_headers_len_bits + 7) / 8;
    if (2 + au_headers_len_bytes > size) {
        std::vector<uint8_t> frame(payload, payload + size);
        EmitPacket(frame, true, rtp_timestamp, seq_num);
        return;
    }

    uint16_t au_header = (static_cast<uint16_t>(payload[2]) << 8) | payload[3];
    size_t au_size = au_header >> 3;
    size_t payload_offset = 2 + au_headers_len_bytes;

    if (payload_offset + au_size <= size && au_size > 0) {
        std::vector<uint8_t> frame(payload + payload_offset, payload + payload_offset + au_size);
        EmitPacket(frame, true, rtp_timestamp, seq_num);
    } else {
        std::vector<uint8_t> frame(payload + payload_offset, payload + size);
        EmitPacket(frame, true, rtp_timestamp, seq_num);
    }
}

void RtpDepacketizer::ProcessG711(const uint8_t* payload, size_t size, uint32_t rtp_timestamp, uint16_t seq_num) {
    if (size == 0) return;
    std::vector<uint8_t> frame(payload, payload + size);
    EmitPacket(frame, true, rtp_timestamp, seq_num);
}

void RtpDepacketizer::EmitPacket(const std::vector<uint8_t>& data, bool is_keyframe, uint32_t rtp_timestamp, uint16_t seq_num) {
    if (!callback_ || data.empty()) return;

    auto packet = std::make_shared<MediaPacket>();
    packet->channel_id = channel_id_;
    packet->stream_type = stream_type_;
    packet->codec = codec_;
    packet->media_type = (codec_ == CodecType::AAC || codec_ == CodecType::PCMA || codec_ == CodecType::PCMU) ?
                         MediaType::AUDIO : MediaType::VIDEO;
    packet->is_keyframe = is_keyframe;
    packet->rtp_timestamp = rtp_timestamp;
    packet->sequence_number = seq_num;
    packet->wall_time_ms = time_utils::WallTimeMs();

    // Convert RTP timestamp to microseconds with seamless 32-bit wrap unrolling
    if (!has_base_pts_) {
        base_pts_us_ = time_utils::MonotonicUs();
        last_rtp_timestamp_ = rtp_timestamp;
        unwrapped_rtp_timestamp_ = 0;
        has_base_pts_ = true;
        packet->pts_us = base_pts_us_;
    } else {
        uint32_t effective_rate = (clock_rate_ > 0) ? clock_rate_ : 90000;
        int32_t delta_ticks = static_cast<int32_t>(rtp_timestamp - last_rtp_timestamp_);
        int64_t max_drift_ticks = static_cast<int64_t>(effective_rate) * 10LL;
        if (delta_ticks < -max_drift_ticks || delta_ticks > max_drift_ticks) {
            // Clock jump >10s detected; re-anchor to monotonic clock
            base_pts_us_ = time_utils::MonotonicUs();
            unwrapped_rtp_timestamp_ = 0;
        } else {
            unwrapped_rtp_timestamp_ += delta_ticks;
        }
        last_rtp_timestamp_ = rtp_timestamp;

        int64_t delta_us = (unwrapped_rtp_timestamp_ * 1000000LL) / static_cast<int64_t>(effective_rate);
        packet->pts_us = base_pts_us_ + delta_us;
    }
    packet->dts_us = packet->pts_us;
    packet->data = data;

    callback_(packet);
}

} // namespace nvr
