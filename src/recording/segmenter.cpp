#include "nvr/recording/segmenter.h"
#include "nvr/storage/segment_index.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

namespace nvr {

Segmenter::Segmenter(int channel_id, const std::string& output_directory, int segment_duration_seconds)
    : channel_id_(channel_id),
      output_dir_(output_directory),
      segment_duration_ms_(segment_duration_seconds * 1000),
      writer_(channel_id, output_directory) {
}

Segmenter::~Segmenter() {
    FlushAndStop();
}

void Segmenter::PushPacket(const MediaPacketPtr& packet) {
    if (!packet) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Audio stream packets are routed directly to the writer when active.
    // Audio never dictates segment rotation and is dropped if waiting for first keyframe.
    if (packet->IsAudio()) {
        if (is_recording_) {
            writer_.WritePacket(packet);
        }
        return;
    }

    // Always inspect and cache incoming parameter sets (SPS, PPS, VPS)
    ExtractAndCacheParameterSets(packet);

    // Segments must always begin on an IDR keyframe
    if (waiting_for_first_keyframe_) {
        if (!packet->is_keyframe) {
            return;
        }
        int64_t now_ms = (packet->wall_time_ms > 0) ? packet->wall_time_ms : time_utils::WallTimeMs();
        if (writer_.StartSegment(now_ms)) {
            is_recording_ = true;
            waiting_for_first_keyframe_ = false;
            InjectCachedParameterSets();
        } else {
            return;
        }
    }

    int64_t elapsed_ms = 0;
    if (writer_.GetSegmentStartPtsUs() >= 0 && packet->pts_us > writer_.GetSegmentStartPtsUs()) {
        elapsed_ms = (packet->pts_us - writer_.GetSegmentStartPtsUs()) / 1000;
    } else {
        int64_t now_ms = (packet->wall_time_ms > 0) ? packet->wall_time_ms : time_utils::WallTimeMs();
        elapsed_ms = now_ms - writer_.GetSegmentStartTimeMs();
    }

    // Check if segment duration reached and packet is a keyframe (GOP alignment)
    if (elapsed_ms >= segment_duration_ms_ && packet->is_keyframe) {
        RotateSegment(packet);
        return;
    }

    // Hard segment ceiling guardrail: if camera fails to send keyframe for 2x target duration,
    // force-finalize current segment to avoid runaway uncommitted file sizes.
    if (elapsed_ms >= (2 * segment_duration_ms_)) {
        LOG_WARN << "[Channel " << channel_id_
                 << "] Missing keyframe timeout (" << elapsed_ms << " ms > "
                 << (2 * segment_duration_ms_) << " ms ceiling). Force-finalizing segment.";
        SegmentMetadata meta;
        if (writer_.FinalizeSegment(meta)) {
            SegmentIndex::Instance().InsertSegment(meta);
        }
        waiting_for_first_keyframe_ = true;
        is_recording_ = false;
        return;
    }

    writer_.WritePacket(packet);
}

void Segmenter::RotateSegment(const MediaPacketPtr& next_keyframe_packet) {
    SegmentMetadata meta;
    if (writer_.FinalizeSegment(meta)) {
        // Commit index to SQLite
        SegmentIndex::Instance().InsertSegment(meta);
    }

    // Immediately start new segment with the incoming keyframe
    int64_t now_ms = (next_keyframe_packet->wall_time_ms > 0) ? 
                     next_keyframe_packet->wall_time_ms : time_utils::WallTimeMs();
    if (writer_.StartSegment(now_ms)) {
        InjectCachedParameterSets();
        writer_.WritePacket(next_keyframe_packet);
    } else {
        waiting_for_first_keyframe_ = true;
        is_recording_ = false;
    }
}

void Segmenter::FlushAndStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_recording_) {
        SegmentMetadata meta;
        if (writer_.FinalizeSegment(meta)) {
            SegmentIndex::Instance().InsertSegment(meta);
        }
        is_recording_ = false;
        waiting_for_first_keyframe_ = true;
        LOG_INFO << "[Channel " << channel_id_ << "] Flushed and stopped segmenter";
    }
}

void Segmenter::ConfigureAudio(CodecType codec, uint32_t sample_rate, uint8_t channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    writer_.ConfigureAudio(codec, sample_rate, channels);
}

bool Segmenter::IsRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_recording_;
}

uint32_t Segmenter::GetCurrentFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writer_.GetFrameCount();
}

uint64_t Segmenter::GetCurrentBytesWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writer_.GetCurrentBytesWritten();
}

void Segmenter::ExtractAndCacheParameterSets(const MediaPacketPtr& packet) {
    if (!packet || packet->data.empty()) return;
    const uint8_t* ptr = packet->data.data();
    size_t len = packet->data.size();

    size_t i = 0;
    while (i + 3 < len) {
        if (ptr[i] == 0 && ptr[i+1] == 0 && (ptr[i+2] == 1 || (ptr[i+2] == 0 && i + 3 < len && ptr[i+3] == 1))) {
            size_t start_code_len = (ptr[i+2] == 1) ? 3 : 4;
            size_t nal_start = i + start_code_len;
            size_t next = nal_start;
            while (next + 2 < len) {
                if (ptr[next] == 0 && ptr[next+1] == 0 && (ptr[next+2] == 1 || (next + 3 < len && ptr[next+2] == 0 && ptr[next+3] == 1))) {
                    break;
                }
                next++;
            }
            if (next + 2 >= len) next = len;
            if (next > nal_start) {
                const uint8_t* nal_data = ptr + nal_start;
                size_t nal_len = next - nal_start;
                if (packet->codec == CodecType::H264) {
                    uint8_t nal_type = nal_data[0] & 0x1F;
                    if (nal_type == 7) { // SPS
                        cached_sps_.assign(nal_data, nal_data + nal_len);
                        cached_codec_ = CodecType::H264;
                    } else if (nal_type == 8) { // PPS
                        cached_pps_.assign(nal_data, nal_data + nal_len);
                        cached_codec_ = CodecType::H264;
                    }
                } else if (packet->codec == CodecType::H265) {
                    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                    if (nal_type == 32) { // VPS
                        cached_vps_.assign(nal_data, nal_data + nal_len);
                        cached_codec_ = CodecType::H265;
                    } else if (nal_type == 33) { // SPS
                        cached_sps_.assign(nal_data, nal_data + nal_len);
                        cached_codec_ = CodecType::H265;
                    } else if (nal_type == 34) { // PPS
                        cached_pps_.assign(nal_data, nal_data + nal_len);
                        cached_codec_ = CodecType::H265;
                    }
                }
            }
            i = next;
            continue;
        }
        i++;
    }
}

void Segmenter::InjectCachedParameterSets() {
    if (cached_sps_.empty() && cached_pps_.empty() && cached_vps_.empty()) {
        return;
    }
    auto param_pkt = std::make_shared<MediaPacket>();
    param_pkt->channel_id = channel_id_;
    param_pkt->codec = cached_codec_;
    param_pkt->is_keyframe = false;
    param_pkt->pts_us = 0;
    param_pkt->dts_us = 0;

    auto append_nal = [&](const std::vector<uint8_t>& nal) {
        if (nal.empty()) return;
        param_pkt->data.push_back(0x00);
        param_pkt->data.push_back(0x00);
        param_pkt->data.push_back(0x00);
        param_pkt->data.push_back(0x01);
        param_pkt->data.insert(param_pkt->data.end(), nal.begin(), nal.end());
    };

    if (cached_codec_ == CodecType::H265) {
        append_nal(cached_vps_);
        append_nal(cached_sps_);
        append_nal(cached_pps_);
    } else {
        append_nal(cached_sps_);
        append_nal(cached_pps_);
    }

    if (!param_pkt->data.empty()) {
        writer_.WritePacket(param_pkt);
    }
}

} // namespace nvr
