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

    // Segments must always begin on an IDR keyframe
    if (waiting_for_first_keyframe_) {
        if (!packet->is_keyframe) {
            return;
        }
        int64_t now_ms = (packet->wall_time_ms > 0) ? packet->wall_time_ms : time_utils::WallTimeMs();
        if (writer_.StartSegment(now_ms)) {
            is_recording_ = true;
            waiting_for_first_keyframe_ = false;
        } else {
            return;
        }
    }

    int64_t now_ms = (packet->wall_time_ms > 0) ? packet->wall_time_ms : time_utils::WallTimeMs();
    int64_t elapsed_ms = now_ms - writer_.GetSegmentStartTimeMs();

    // Check if segment duration reached and packet is a keyframe (GOP alignment)
    if (elapsed_ms >= segment_duration_ms_ && packet->is_keyframe) {
        RotateSegment(packet);
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

bool Segmenter::IsRecording() const {
    return is_recording_;
}

uint32_t Segmenter::GetCurrentFrameCount() const {
    return writer_.GetFrameCount();
}

uint64_t Segmenter::GetCurrentBytesWritten() const {
    return writer_.GetCurrentBytesWritten();
}

} // namespace nvr
