#include "nvr/playback/seek_controller.h"
#include "nvr/common/logger.h"
#include <algorithm>
#include <ctime>

namespace nvr {

namespace {

int64_t ParseIsoToUtcMs(const std::string& iso_str) {
    if (iso_str.empty()) return 0;
    struct tm tm{};
    if (strptime(iso_str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        return static_cast<int64_t>(timegm(&tm)) * 1000LL;
    }
    return 0;
}

} // namespace

SeekController::SeekController() {
    current_pos_.channel_id = 1;
    current_pos_.speed = 1.0;
    current_pos_.mode = PlaybackMode::STOPPED;
}

void SeekController::SetTimeline(int channel_id, const std::vector<nlohmann::json>& segments) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_id_ = channel_id;
    segments_ = segments;

    std::sort(segments_.begin(), segments_.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("start_time", "") < b.value("start_time", "");
    });

    total_duration_sec_ = 0.0;
    timeline_start_utc_ms_ = 0;
    timeline_end_utc_ms_ = 0;

    if (!segments_.empty()) {
        std::string s_first = segments_.front().value("start_time", "");
        std::string s_last = segments_.back().value("end_time", "");
        timeline_start_utc_ms_ = ParseIsoToUtcMs(s_first);
        timeline_end_utc_ms_ = ParseIsoToUtcMs(s_last);

        for (const auto& seg : segments_) {
            total_duration_sec_ += seg.value("duration_seconds", 60.0);
        }

        current_pos_.channel_id = channel_id_;
        current_pos_.current_utc_ms = timeline_start_utc_ms_;
        current_pos_.current_file_path = segments_.front().value("file_path", "");
        current_pos_.file_offset_ms = 0;
        current_pos_.current_segment_index = 0;
        current_pos_.total_segments = static_cast<int>(segments_.size());
        current_pos_.total_duration_sec = total_duration_sec_;
    } else {
        current_pos_ = PlaybackPosition();
        current_pos_.channel_id = channel_id_;
    }
}

int SeekController::FindSegmentIndexByUtc(int64_t target_utc_ms) const {
    if (segments_.empty()) return -1;

    for (size_t i = 0; i < segments_.size(); ++i) {
        int64_t s_start = ParseIsoToUtcMs(segments_[i].value("start_time", ""));
        int64_t s_end = ParseIsoToUtcMs(segments_[i].value("end_time", ""));
        if (s_end <= s_start) {
            s_end = s_start + static_cast<int64_t>(segments_[i].value("duration_seconds", 60.0) * 1000.0);
        }
        if (target_utc_ms >= s_start && target_utc_ms <= s_end) {
            return static_cast<int>(i);
        }
    }

    if (target_utc_ms < timeline_start_utc_ms_) return 0;
    return static_cast<int>(segments_.size() - 1);
}

bool SeekController::SeekToUtc(int64_t target_utc_ms, PlaybackPosition& out_pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (segments_.empty()) return false;

    int idx = FindSegmentIndexByUtc(target_utc_ms);
    if (idx < 0 || idx >= static_cast<int>(segments_.size())) return false;

    const auto& seg = segments_[idx];
    int64_t s_start = ParseIsoToUtcMs(seg.value("start_time", ""));
    int64_t offset_ms = std::max<int64_t>(0, target_utc_ms - s_start);

    current_pos_.channel_id = channel_id_;
    current_pos_.current_utc_ms = target_utc_ms;
    current_pos_.current_file_path = seg.value("file_path", "");
    current_pos_.file_offset_ms = offset_ms;
    current_pos_.current_segment_index = idx;
    current_pos_.total_segments = static_cast<int>(segments_.size());
    current_pos_.total_duration_sec = total_duration_sec_;

    out_pos = current_pos_;
    return true;
}

bool SeekController::SeekToSeconds(double seconds_from_start, PlaybackPosition& out_pos) {
    int64_t target_utc = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (segments_.empty() || timeline_start_utc_ms_ <= 0) return false;
        target_utc = timeline_start_utc_ms_ + static_cast<int64_t>(seconds_from_start * 1000.0);
    }
    return SeekToUtc(target_utc, out_pos);
}

bool SeekController::StepGop(bool forward, PlaybackPosition& out_pos) {
    int64_t target_utc = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (segments_.empty()) return false;

        int64_t delta = forward ? 1000LL : -1000LL;
        target_utc = current_pos_.current_utc_ms + delta;
        if (target_utc < timeline_start_utc_ms_) target_utc = timeline_start_utc_ms_;
        if (target_utc > timeline_end_utc_ms_) target_utc = timeline_end_utc_ms_;
    }
    return SeekToUtc(target_utc, out_pos);
}

void SeekController::SetSpeed(double speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (speed <= 0.0) speed = 1.0;
    current_pos_.speed = speed;
}

double SeekController::GetSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_pos_.speed;
}

void SeekController::SetMode(PlaybackMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_pos_.mode = mode;
}

PlaybackMode SeekController::GetMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_pos_.mode;
}

PlaybackPosition SeekController::GetCurrentPosition() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_pos_;
}

double SeekController::GetTotalDurationSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_duration_sec_;
}

int SeekController::GetSegmentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(segments_.size());
}

} // namespace nvr
