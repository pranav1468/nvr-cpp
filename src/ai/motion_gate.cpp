#include "nvr/ai/motion_gate.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/logger.h"

#include <cmath>
#include <algorithm>

namespace nvr {

MotionGate::MotionGate(const MotionGateConfig& config)
    : config_(config) {
}

void MotionGate::Configure(const MotionGateConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    has_reference_ = false;
    reference_grid_.clear();
    current_grid_.clear();
}

MotionGateConfig MotionGate::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void MotionGate::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_reference_ = false;
    reference_grid_.clear();
    current_grid_.clear();
    motion_active_ = false;
    last_motion_time_ms_ = 0;
    last_eval_time_ms_ = 0;
}

bool MotionGate::IsMotionActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
        return false;
    }
    int64_t now_ms = time_utils::WallTimeMs();
    return motion_active_ && (now_ms - last_motion_time_ms_ <= config_.hold_time_ms);
}

MotionGateResult MotionGate::ProcessFrame(const DecodedFramePtr& frame) {
    if (!frame || frame->data.empty() || frame->width == 0 || frame->height == 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    MotionGateResult result;
    if (!config_.enabled) {
        return result;
    }

    int64_t now_ms = frame->wall_time_ms > 0 ? frame->wall_time_ms : time_utils::WallTimeMs();
    result.timestamp_ms = now_ms;

    // Rate-limit processing to min_interval_ms to keep CPU near zero (< 0.5% per channel)
    if (last_eval_time_ms_ > 0 && (now_ms - last_eval_time_ms_ < config_.min_interval_ms)) {
        result.is_motion_active = motion_active_ && (now_ms - last_motion_time_ms_ <= config_.hold_time_ms);
        return result;
    }
    last_eval_time_ms_ = now_ms;

    int cols = std::max(4, std::min(128, config_.grid_cols));
    int rows = std::max(4, std::min(96, config_.grid_rows));
    int total_cells = cols * rows;
    result.total_cells = total_cells;

    if (current_grid_.size() != static_cast<size_t>(total_cells)) {
        current_grid_.resize(total_cells);
    }

    const uint8_t* raw_data = frame->data.data();
    uint32_t stride = frame->stride > 0 ? frame->stride : frame->width;

    // Fast integer-only macroblock center sampling
    if (frame->format == PixelFormat::NV12 || frame->format == PixelFormat::YUV420P) {
        // First plane is luminance (Y)
        for (int r = 0; r < rows; ++r) {
            uint32_t sample_y = (static_cast<uint32_t>(r) * frame->height + frame->height / 2) / rows;
            if (sample_y >= frame->height) sample_y = frame->height - 1;
            const uint8_t* row_ptr = raw_data + sample_y * stride;

            for (int c = 0; c < cols; ++c) {
                uint32_t sample_x = (static_cast<uint32_t>(c) * frame->width + frame->width / 2) / cols;
                if (sample_x >= frame->width) sample_x = frame->width - 1;
                current_grid_[r * cols + c] = row_ptr[sample_x];
            }
        }
    } else {
        // RGB/RGBA fallback: convert RGB to approximate luma via fast fixed-point (2R + 5G + B) >> 3
        int bpp = (frame->format == PixelFormat::RGBA32) ? 4 : 3;
        for (int r = 0; r < rows; ++r) {
            uint32_t sample_y = (static_cast<uint32_t>(r) * frame->height + frame->height / 2) / rows;
            if (sample_y >= frame->height) sample_y = frame->height - 1;
            const uint8_t* row_ptr = raw_data + sample_y * (stride > 0 ? stride : frame->width * bpp);

            for (int c = 0; c < cols; ++c) {
                uint32_t sample_x = (static_cast<uint32_t>(c) * frame->width + frame->width / 2) / cols;
                if (sample_x >= frame->width) sample_x = frame->width - 1;
                const uint8_t* px = row_ptr + sample_x * bpp;
                int y_val = (static_cast<int>(px[0]) * 2 + static_cast<int>(px[1]) * 5 + static_cast<int>(px[2])) >> 3;
                current_grid_[r * cols + c] = static_cast<uint8_t>(y_val);
            }
        }
    }

    // Initialize reference background on first frame or resolution/grid change
    if (!has_reference_ || reference_grid_.size() != static_cast<size_t>(total_cells)) {
        reference_grid_ = current_grid_;
        has_reference_ = true;
        result.motion_detected = false;
        result.is_motion_active = false;
        return result;
    }

    // Integer differencing against background reference
    int changed_cells = 0;
    for (int i = 0; i < total_cells; ++i) {
        int diff = std::abs(static_cast<int>(current_grid_[i]) - static_cast<int>(reference_grid_[i]));
        if (diff >= config_.pixel_delta_threshold) {
            changed_cells++;
        }
    }

    float score = static_cast<float>(changed_cells) / static_cast<float>(total_cells);
    bool detected = (score >= config_.motion_area_ratio);

    if (detected) {
        last_motion_time_ms_ = now_ms;
        motion_active_ = true;
    } else {
        if (now_ms - last_motion_time_ms_ > config_.hold_time_ms) {
            motion_active_ = false;
        }
        // Adaptive ambient background update (only when no foreground motion is detected):
        // Running average formula: (ref * 7 + curr + 4) >> 3
        for (int i = 0; i < total_cells; ++i) {
            reference_grid_[i] = static_cast<uint8_t>(
                (static_cast<int>(reference_grid_[i]) * 7 + static_cast<int>(current_grid_[i]) + 4) >> 3
            );
        }
    }

    result.motion_detected = detected;
    result.is_motion_active = motion_active_;
    result.motion_score = score;
    result.changed_cells = changed_cells;
    return result;
}

// -----------------------------------------------------------------------------
// MotionGateManager Implementation
// -----------------------------------------------------------------------------

MotionGateManager& MotionGateManager::Instance() {
    static MotionGateManager instance;
    return instance;
}

void MotionGateManager::ConfigureChannel(int channel_id, const MotionGateConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gates_.find(channel_id);
    if (it != gates_.end()) {
        it->second->Configure(config);
    } else {
        auto gate = std::make_unique<MotionGate>(config);
        gates_[channel_id] = std::move(gate);
    }
}

MotionGateConfig MotionGateManager::GetChannelConfig(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gates_.find(channel_id);
    if (it != gates_.end()) {
        return it->second->GetConfig();
    }
    return MotionGateConfig{};
}

MotionGateResult MotionGateManager::ProcessFrame(int channel_id, const DecodedFramePtr& frame) {
    MotionGate* gate = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gates_.find(channel_id);
        if (it == gates_.end()) {
            auto new_gate = std::make_unique<MotionGate>();
            gate = new_gate.get();
            gates_[channel_id] = std::move(new_gate);
        } else {
            gate = it->second.get();
        }
    }

    MotionGateResult result = gate->ProcessFrame(frame);
    RecordingScheduler::Instance().SetMotionEvent(channel_id, result.is_motion_active);
    return result;
}

bool MotionGateManager::IsMotionActive(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gates_.find(channel_id);
    if (it != gates_.end()) {
        return it->second->IsMotionActive();
    }
    return false;
}

void MotionGateManager::ResetChannel(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gates_.find(channel_id);
    if (it != gates_.end()) {
        it->second->Reset();
    }
}

void MotionGateManager::RemoveChannel(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    gates_.erase(channel_id);
}

} // namespace nvr
