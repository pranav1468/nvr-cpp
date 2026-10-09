#include "nvr/live/live_queue.h"
#include "nvr/media/buffer_allocator.h"

namespace nvr {

LiveQueue::LiveQueue(size_t max_depth) : max_depth_(max_depth > 0 ? max_depth : 2) {}

LiveQueue::~LiveQueue() {
    Stop();
    Clear();
}

bool LiveQueue::Push(DecodedFramePtr frame) {
    if (!frame || stopped_) {
        return false;
    }

    DecodedFramePtr dropped_frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }

        // Head-drop policy: Drop the oldest frame at the head when capacity is reached
        while (queue_.size() >= max_depth_) {
            dropped_frame = queue_.front();
            queue_.pop_front();
            total_dropped_++;
            if (dropped_frame && !dropped_frame->data.empty()) {
                BufferAllocator::Instance().Release(std::move(dropped_frame->data));
            }
        }

        queue_.push_back(frame);
        total_enqueued_++;
    }

    cv_.notify_one();
    return true;
}

bool LiveQueue::Pop(DecodedFramePtr& out_frame, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        if (stopped_) {
            return false;
        }
        if (timeout_ms <= 0) {
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return !queue_.empty() || stopped_;
        });
    }

    if (queue_.empty() || stopped_) {
        return false;
    }

    out_frame = queue_.front();
    queue_.pop_front();
    total_popped_++;
    return true;
}

bool LiveQueue::PopLatest(DecodedFramePtr& out_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty() || stopped_) {
        return false;
    }

    // Drain all intermediate stale frames to guarantee zero latency
    while (queue_.size() > 1) {
        auto stale = queue_.front();
        queue_.pop_front();
        total_dropped_++;
        if (stale && !stale->data.empty()) {
            BufferAllocator::Instance().Release(std::move(stale->data));
        }
    }

    out_frame = queue_.front();
    queue_.pop_front();
    total_popped_++;
    return true;
}

void LiveQueue::Stop() {
    stopped_ = true;
    cv_.notify_all();
}

void LiveQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        auto f = queue_.front();
        queue_.pop_front();
        if (f && !f->data.empty()) {
            BufferAllocator::Instance().Release(std::move(f->data));
        }
    }
}

size_t LiveQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

size_t LiveQueue::GetMaxDepth() const {
    return max_depth_;
}

void LiveQueue::SetMaxDepth(size_t max_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_depth_ = max_depth > 0 ? max_depth : 2;
}

uint64_t LiveQueue::GetTotalEnqueued() const {
    return total_enqueued_.load();
}

uint64_t LiveQueue::GetTotalDropped() const {
    return total_dropped_.load();
}

uint64_t LiveQueue::GetTotalPopped() const {
    return total_popped_.load();
}

} // namespace nvr
