#include "nvr/media/buffer_allocator.h"
#include <algorithm>

namespace nvr {

BufferAllocator& BufferAllocator::Instance() {
    static BufferAllocator instance;
    return instance;
}

BufferAllocator::~BufferAllocator() {
    ClearPool();
}

AlignedByteBuffer BufferAllocator::Allocate(size_t size) {
    if (size == 0) {
        return {};
    }

    // Align request size to 64 bytes
    size_t aligned_size = (size + 63) & ~size_t(63);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& pool = pools_[aligned_size];
    if (!pool.empty()) {
        AlignedByteBuffer buf = std::move(pool.back());
        pool.pop_back();
        buf.resize(size);
        return buf;
    }

    total_allocated_bytes_ += aligned_size;
    if (total_allocated_bytes_ > peak_allocated_bytes_) {
        peak_allocated_bytes_ = total_allocated_bytes_.load();
    }

    AlignedByteBuffer buf;
    buf.reserve(aligned_size);
    buf.resize(size);
    return buf;
}

DecodedFramePtr BufferAllocator::AllocateFrame(uint32_t width, uint32_t height, PixelFormat format) {
    size_t req_bytes = 0;
    if (format == PixelFormat::NV12 || format == PixelFormat::YUV420P) {
        req_bytes = CalculateSurfaceSize(width, height, 1, 1, 64);
    } else if (format == PixelFormat::RGB24) {
        req_bytes = static_cast<size_t>(width) * height * 3;
    } else if (format == PixelFormat::RGBA32) {
        req_bytes = static_cast<size_t>(width) * height * 4;
    } else {
        req_bytes = CalculateSurfaceSize(width, height, 1, 1, 64);
    }

    AlignedByteBuffer buf = Allocate(req_bytes);
    auto* raw_frame = new DecodedFrame();
    raw_frame->width = width;
    raw_frame->height = height;
    raw_frame->format = format;
    raw_frame->stride = (width + 63) & ~63;
    raw_frame->data = std::move(buf);

    return DecodedFramePtr(raw_frame, [](DecodedFrame* f) {
        if (f) {
            BufferAllocator::Instance().Release(std::move(f->data));
            delete f;
        }
    });
}

void BufferAllocator::Release(AlignedByteBuffer&& buffer) {
    if (buffer.empty()) {
        return;
    }

    size_t cap = buffer.capacity();
    size_t aligned_size = (cap + 63) & ~size_t(63);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& pool = pools_[aligned_size];
    if (pool.size() < max_pool_entries_per_size_) {
        buffer.clear();
        pool.push_back(std::move(buffer));
    } else {
        // Pool capacity reached, let it destruct and adjust tracking
        if (total_allocated_bytes_ >= aligned_size) {
            total_allocated_bytes_ -= aligned_size;
        }
        buffer.reset();
    }
}

size_t BufferAllocator::CalculateSurfaceSize(uint32_t width, uint32_t height, size_t bpp_numerator, size_t bpp_denominator, size_t alignment) {
    // Aligned stride calculation: strideY * H + strideUV * (H / 2)
    size_t stride_y = (width + alignment - 1) & ~(alignment - 1);
    size_t size_y = stride_y * height;
    size_t size_uv = (stride_y * (height / 2) * bpp_numerator) / bpp_denominator;
    return size_y + size_uv;
}

size_t BufferAllocator::GetPoolCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [sz, vec] : pools_) {
        total += vec.size();
    }
    return total;
}

size_t BufferAllocator::GetTotalAllocatedBytes() const {
    return total_allocated_bytes_.load();
}

size_t BufferAllocator::GetPeakAllocatedBytes() const {
    return peak_allocated_bytes_.load();
}

void BufferAllocator::ClearPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    pools_.clear();
    total_allocated_bytes_ = 0;
}

} // namespace nvr
