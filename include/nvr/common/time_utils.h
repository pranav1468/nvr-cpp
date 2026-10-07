#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace nvr {
namespace time_utils {

inline int64_t MonotonicUs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

inline int64_t MonotonicMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

inline int64_t MonotonicSec() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

inline int64_t WallTimeMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

inline int64_t WallTimeSec() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string FormatIso8601(int64_t epoch_ms);

std::string FormatTimestampCompact(int64_t epoch_ms);

} // namespace time_utils
} // namespace nvr
