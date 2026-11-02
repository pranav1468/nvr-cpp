#pragma once

#include "nvr/common/types.h"
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdint>

namespace nvr {

struct MotionGateConfig {
    int grid_cols{32};               // Macroblock grid columns (default 32)
    int grid_rows{24};               // Macroblock grid rows (default 24)
    int pixel_delta_threshold{25};   // Min luma intensity difference (0..255) to mark a cell changed
    float motion_area_ratio{0.015f}; // Ratio of changed cells to trigger motion (1.5% of grid)
    int min_interval_ms{100};        // Minimum evaluation interval (e.g., max 10 fps)
    int hold_time_ms{2000};          // Hold motion active for at least 2000ms after motion ceases
    bool enabled{true};
};

struct MotionGateResult {
    bool motion_detected{false};     // True if motion detected on this frame
    bool is_motion_active{false};    // True if motion is detected or in hold-down period
    float motion_score{0.0f};        // Ratio of changed cells (0.0 .. 1.0)
    int changed_cells{0};            // Number of changed macroblocks
    int total_cells{0};              // Total number of macroblocks (cols * rows)
    int64_t timestamp_ms{0};         // Evaluation wall time
};

class MotionGate {
public:
    explicit MotionGate(const MotionGateConfig& config = MotionGateConfig{});
    ~MotionGate() = default;

    void Configure(const MotionGateConfig& config);
    MotionGateConfig GetConfig() const;

    // Evaluates a decoded frame (Y-plane) and returns detection result
    MotionGateResult ProcessFrame(const DecodedFramePtr& frame);

    // Reset background reference model
    void Reset();

    // Check if motion is currently active (including hold time)
    bool IsMotionActive() const;

private:
    MotionGateConfig config_;
    mutable std::mutex mutex_;

    std::vector<uint8_t> reference_grid_;
    std::vector<uint8_t> current_grid_;
    bool has_reference_{false};
    int64_t last_eval_time_ms_{0};
    int64_t last_motion_time_ms_{0};
    bool motion_active_{false};
};

class MotionGateManager {
public:
    static MotionGateManager& Instance();

    void ConfigureChannel(int channel_id, const MotionGateConfig& config);
    MotionGateConfig GetChannelConfig(int channel_id) const;

    // Feed a frame into a channel's motion gate; if motion triggers and no ONVIF event worker is online,
    // automatically forwards event to RecordingScheduler::Instance().SetMotionEvent(channel_id, active).
    MotionGateResult ProcessFrame(int channel_id, const DecodedFramePtr& frame);

    bool IsMotionActive(int channel_id) const;
    void ResetChannel(int channel_id);
    void RemoveChannel(int channel_id);

private:
    MotionGateManager() = default;
    ~MotionGateManager() = default;
    MotionGateManager(const MotionGateManager&) = delete;
    MotionGateManager& operator=(const MotionGateManager&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<int, std::unique_ptr<MotionGate>> gates_;
};

} // namespace nvr
