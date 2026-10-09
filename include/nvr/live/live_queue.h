#pragma once

#include "nvr/common/types.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace nvr {

class LiveQueue {
public:
    explicit LiveQueue(size_t max_depth = 2);
    ~LiveQueue();

    // Push frame into queue. Enforces head-drop (drop-stale) policy when size >= max_depth.
    bool Push(DecodedFramePtr frame);

    // Pop the next available frame (blocking with timeout)
    bool Pop(DecodedFramePtr& out_frame, int timeout_ms = 100);

    // Drain all stale frames and immediately return the freshest tail frame
    bool PopLatest(DecodedFramePtr& out_frame);

    // Stop queue and unblock any waiting pop calls
    void Stop();

    // Reset queue state
    void Clear();

    // Queue depth and telemetry metrics
    size_t Size() const;
    size_t GetMaxDepth() const;
    void SetMaxDepth(size_t max_depth);

    uint64_t GetTotalEnqueued() const;
    uint64_t GetTotalDropped() const;
    uint64_t GetTotalPopped() const;

private:
    size_t max_depth_{2};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<DecodedFramePtr> queue_;
    std::atomic<bool> stopped_{false};

    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_popped_{0};
};

} // namespace nvr
