#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include "3rdparty/nlohmann/json.hpp"

namespace nvr {

enum class PlaybackMode {
    STOPPED = 0,
    PLAYING,
    PAUSED,
    FAST_FORWARD,
    SLOW_MOTION,
    REVERSE
};

inline const char* PlaybackModeToString(PlaybackMode mode) {
    switch (mode) {
        case PlaybackMode::PLAYING:      return "PLAYING";
        case PlaybackMode::PAUSED:       return "PAUSED";
        case PlaybackMode::FAST_FORWARD: return "FAST_FORWARD";
        case PlaybackMode::SLOW_MOTION:  return "SLOW_MOTION";
        case PlaybackMode::REVERSE:      return "REVERSE";
        default:                         return "STOPPED";
    }
}

struct PlaybackPosition {
    int channel_id{1};
    int64_t current_utc_ms{0};
    std::string current_file_path;
    int64_t file_offset_ms{0};
    double speed{1.0};
    PlaybackMode mode{PlaybackMode::STOPPED};
    int current_segment_index{-1};
    int total_segments{0};
    double total_duration_sec{0.0};
};

class SeekController {
public:
    SeekController();
    ~SeekController() = default;

    // Load segments for a continuous timeline
    void SetTimeline(int channel_id, const std::vector<nlohmann::json>& segments);

    // Seek to an exact wall-clock UTC timestamp in milliseconds
    bool SeekToUtc(int64_t target_utc_ms, PlaybackPosition& out_pos);

    // Seek to seconds from start of the timeline
    bool SeekToSeconds(double seconds_from_start, PlaybackPosition& out_pos);

    // Jump to next or previous GOP / segment boundary
    bool StepGop(bool forward, PlaybackPosition& out_pos);

    // Speed and state control
    void SetSpeed(double speed);
    double GetSpeed() const;

    void SetMode(PlaybackMode mode);
    PlaybackMode GetMode() const;

    PlaybackPosition GetCurrentPosition() const;

    // Query timeline metadata
    double GetTotalDurationSeconds() const;
    int GetSegmentCount() const;

private:
    int FindSegmentIndexByUtc(int64_t target_utc_ms) const;

    mutable std::mutex mutex_;
    int channel_id_{1};
    std::vector<nlohmann::json> segments_;
    PlaybackPosition current_pos_;
    int64_t timeline_start_utc_ms_{0};
    int64_t timeline_end_utc_ms_{0};
    double total_duration_sec_{0.0};
};

} // namespace nvr
