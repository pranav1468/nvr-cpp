#pragma once

#include "nvr/common/types.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace nvr {

class BufferAllocator {
public:
    static BufferAllocator& Instance();

    // Allocate an aligned memory buffer from pool or heap (64-byte alignment)
    AlignedByteBuffer Allocate(size_t size);

    // Return buffer to pool for reuse
    void Release(AlignedByteBuffer&& buffer);

    // Physical surface calculation based on NXP i.MX 95 VPU strides
    static size_t CalculateSurfaceSize(uint32_t width, uint32_t height, size_t bpp_numerator, size_t bpp_denominator, size_t alignment = 64);

    // Telemetry and validation metrics
    size_t GetPoolCount() const;
    size_t GetTotalAllocatedBytes() const;
    size_t GetPeakAllocatedBytes() const;
    void ClearPool();

private:
    BufferAllocator() = default;
    ~BufferAllocator();
    BufferAllocator(const BufferAllocator&) = delete;
    BufferAllocator& operator=(const BufferAllocator&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<size_t, std::vector<AlignedByteBuffer>> pools_;
    std::atomic<size_t> total_allocated_bytes_{0};
    std::atomic<size_t> peak_allocated_bytes_{0};
    const size_t max_pool_entries_per_size_{16};
};

} // namespace nvr
