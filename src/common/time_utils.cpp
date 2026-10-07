#include "nvr/common/time_utils.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace nvr {
namespace time_utils {

std::string FormatIso8601(int64_t epoch_ms) {
    std::time_t sec = static_cast<std::time_t>(epoch_ms / 1000);
    int ms = static_cast<int>(epoch_ms % 1000);

    std::tm tm_buf{};
    localtime_r(&sec, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

std::string FormatTimestampCompact(int64_t epoch_ms) {
    std::time_t sec = static_cast<std::time_t>(epoch_ms / 1000);
    int ms = static_cast<int>(epoch_ms % 1000);

    std::tm tm_buf{};
    localtime_r(&sec, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
        << '_' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

std::string FormatDateOnly(int64_t epoch_ms) {
    std::time_t sec = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_buf{};
    localtime_r(&sec, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}

} // namespace time_utils
} // namespace nvr
