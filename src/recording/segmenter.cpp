#include "nvr/recording/segmenter.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/api/fmp4_muxer.h"
#include "nvr/live/live_controller.h"
#include "nvr/media/image_utils.h"
#include "nvr/database/database_manager.h"

namespace nvr {

namespace {

inline void CaptureSegmentThumbnail(int channel_id, SegmentMetadata& meta) {
    if (meta.file_path.size() <= 4) return;
    auto frame = LiveController::Instance().GetLatestFrame(channel_id);
    if (frame && !frame->data.empty() && frame->width > 0 && frame->height > 0) {
        std::string thumb_path = meta.file_path.substr(0, meta.file_path.size() - 4) + ".jpg";
        if (SaveFrameToJpeg(frame, thumb_path, 75, 320)) {
            meta.thumbnail_path = thumb_path;
            LOG_DEBUG << "[Channel " << channel_id << "] Saved companion thumbnail: " << thumb_path;
        }
    }
}

inline void IndexSegmentInDatabase(int channel_id, const SegmentMetadata& meta) {
    if (meta.file_path.empty()) return;
    RecordingSegmentRecord record;
    record.channel_id = channel_id;
    record.start_time_ms = meta.start_time_ms > 0 ? meta.start_time_ms : time_utils::WallTimeMs();
    record.end_time_ms = meta.end_time_ms > 0 ? meta.end_time_ms : (record.start_time_ms + meta.duration_ms);
    record.duration_sec = meta.duration_ms > 0 ? static_cast<int>(meta.duration_ms / 1000) : 60;
    record.file_path = meta.file_path;
    record.file_size_bytes = static_cast<int64_t>(meta.file_size_bytes);
    record.codec = (meta.codec == CodecType::H265) ? "H265" : "H264";
    record.trigger_type = "continuous";
    record.is_locked = meta.is_locked;
    record.created_at_ms = record.start_time_ms;

    if (!DatabaseManager::Instance().InsertRecordingSegment(record)) {
        LOG_WARN << "[Channel " << channel_id << "] Failed to index recording segment in database: " << meta.file_path;
    } else {
        LOG_DEBUG << "[Channel " << channel_id << "] Indexed segment in database: " << meta.file_path;
    }
}

} // namespace

Segmenter::Segmenter(int channel_id, const std::string& output_directory, int segment_duration_seconds)
    : channel_id_(channel_id),
      output_dir_(output_directory),
      segment_duration_ms_(segment_duration_seconds * 1000),
      writer_(channel_id, output_directory) {
}

Segmenter::~Segmenter() {
    FlushAndStop();
}

void Segmenter::SetOutputDirectory(const std::string& new_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!new_dir.empty()) {
        output_dir_ = new_dir;
        writer_.SetOutputDirectory(new_dir);
    }
}

void Segmenter::PushPacket(const MediaPacketPtr& packet) {
    if (!packet) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Audio stream packets are routed directly to the writer when active.
    // Audio never dictates segment rotation and is dropped if waiting for first keyframe.
    if (packet->IsAudio()) {
        if (!has_audio_configured_ && packet->codec != CodecType::UNKNOWN) {
            writer_.ConfigureAudio(
                packet->codec,
                packet->sample_rate > 0 ? packet->sample_rate : 8000,
                packet->channels > 0 ? packet->channels : 1
            );
            has_audio_configured_ = true;
        }
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

        if (!has_audio_configured_) {
            CodecType acodec = CodecType::UNKNOWN;
            uint32_t asrate = 8000;
            uint8_t ach = 1;
            if (StreamBroker::Instance().GetCachedAudioParams(channel_id_, StreamType::MAIN, acodec, asrate, ach) &&
                acodec != CodecType::UNKNOWN) {
                writer_.ConfigureAudio(acodec, asrate, ach);
                has_audio_configured_ = true;
            }
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

    // Check if stream resolution changed dynamically (whether in a standalone SPS packet or combined keyframe)
    bool resolution_changed = (writer_.GetFrameCount() > 0 &&
                               cached_width_ > 0 && cached_height_ > 0 &&
                               writer_.GetWidth() > 0 && writer_.GetHeight() > 0 &&
                               (cached_width_ != writer_.GetWidth() || cached_height_ != writer_.GetHeight()));

    if (resolution_changed && !packet->is_keyframe) {
        // Standalone SPS/PPS packet arrived ahead of the new-resolution IDR keyframe.
        // Parameter sets are already cached in cached_sps_/cached_pps_ and will be injected on keyframe rotation.
        return;
    }

    // Check if segment duration reached or resolution changed on a keyframe (GOP alignment)
    if (packet->is_keyframe && (elapsed_ms >= segment_duration_ms_ || resolution_changed)) {
        if (resolution_changed) {
            LOG_INFO << "[Channel " << channel_id_ << "] Camera resolution changed dynamically from "
                     << writer_.GetWidth() << "x" << writer_.GetHeight() << " to "
                     << cached_width_ << "x" << cached_height_ << "; rotating segment";
        }
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
            CaptureSegmentThumbnail(channel_id_, meta);
            IndexSegmentInDatabase(channel_id_, meta);
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
        CaptureSegmentThumbnail(channel_id_, meta);
        IndexSegmentInDatabase(channel_id_, meta);
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
            CaptureSegmentThumbnail(channel_id_, meta);
            IndexSegmentInDatabase(channel_id_, meta);
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

    CodecType detected = (packet->codec != CodecType::UNKNOWN)
        ? packet->codec
        : DetectNalCodecFromParamSets(ptr, len);

    if (cached_codec_ != CodecType::UNKNOWN && detected != CodecType::UNKNOWN && cached_codec_ != detected) {
        cached_vps_.clear();
        cached_sps_.clear();
        cached_pps_.clear();
    }
    if (detected != CodecType::UNKNOWN) {
        cached_codec_ = detected;
        const_cast<MediaPacket*>(packet.get())->codec = detected;
    }
    if (cached_codec_ == CodecType::H264) {
        cached_vps_.clear();
    }

    CodecType eff_codec = (cached_codec_ != CodecType::UNKNOWN) ? cached_codec_ : detected;

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
                uint8_t byte0 = nal_data[0];
                if ((byte0 & 0x80) == 0) {
                    if (eff_codec == CodecType::H265 || (eff_codec == CodecType::UNKNOWN && nal_len >= 2 && IsValidH265NalHeader(byte0, nal_data[1]))) {
                        if (nal_len >= 2) {
                            uint8_t nal_type = (byte0 >> 1) & 0x3F;
                            if (nal_type == 32) { cached_vps_.assign(nal_data, nal_data + nal_len); cached_codec_ = CodecType::H265; }
                            else if (nal_type == 33) {
                                cached_sps_.assign(nal_data, nal_data + nal_len);
                                cached_codec_ = CodecType::H265;
                                uint32_t w = 0, h = 0;
                                if (Fmp4Muxer::ParseH265SpsDimensions(nal_data, nal_len, w, h) && w > 0 && h > 0) {
                                    cached_width_ = w;
                                    cached_height_ = h;
                                }
                            }
                            else if (nal_type == 34) { cached_pps_.assign(nal_data, nal_data + nal_len); cached_codec_ = CodecType::H265; }
                        }
                    } else if (eff_codec == CodecType::H264 || (eff_codec == CodecType::UNKNOWN && ((byte0 >> 5) & 0x03) != 0)) {
                        uint8_t nal_type = byte0 & 0x1F;
                        uint8_t nal_ref_idc = (byte0 >> 5) & 0x03;
                        if (nal_type == 7 && nal_ref_idc != 0) {
                            cached_sps_.assign(nal_data, nal_data + nal_len);
                            cached_codec_ = CodecType::H264;
                            cached_vps_.clear();
                            uint32_t w = 0, h = 0;
                            if (Fmp4Muxer::ParseSpsDimensions(nal_data, nal_len, w, h) && w > 0 && h > 0) {
                                cached_width_ = w;
                                cached_height_ = h;
                            }
                        }
                        else if (nal_type == 8 && nal_ref_idc != 0) { cached_pps_.assign(nal_data, nal_data + nal_len); cached_codec_ = CodecType::H264; cached_vps_.clear(); }
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
    } else if (cached_codec_ == CodecType::H264) {
        append_nal(cached_sps_);
        append_nal(cached_pps_);
    } else {
        if (!cached_vps_.empty()) {
            param_pkt->codec = CodecType::H265;
            append_nal(cached_vps_);
        }
        append_nal(cached_sps_);
        append_nal(cached_pps_);
    }

    if (!param_pkt->data.empty()) {
        writer_.WritePacket(param_pkt);
    }
}

} // namespace nvr
