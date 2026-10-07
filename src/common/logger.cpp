#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <cstring>
#include <cstdio>

namespace nvr {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
}

LogLevel Logger::GetLogLevel() const {
    return current_level_;
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& message) {
    if (level < current_level_) {
        return;
    }

    const char* level_str = "INFO";
    const char* color_code = "\033[0m";

    switch (level) {
        case LogLevel::DEBUG:
            level_str = "DEBUG";
            color_code = "\033[36m"; // Cyan
            break;
        case LogLevel::INFO:
            level_str = "INFO ";
            color_code = "\033[32m"; // Green
            break;
        case LogLevel::WARN:
            level_str = "WARN ";
            color_code = "\033[33m"; // Yellow
            break;
        case LogLevel::ERROR:
            level_str = "ERROR";
            color_code = "\033[31m"; // Red
            break;
    }

    // Basename of file
    const char* base_file = strrchr(file, '/');
    base_file = (base_file != nullptr) ? (base_file + 1) : file;

    std::string timestamp = time_utils::FormatIso8601(time_utils::WallTimeMs());

    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stderr, "%s[%s] [%s] [%s:%d]\033[0m %s\n",
                 color_code, timestamp.c_str(), level_str, base_file, line, message.c_str());
    std::fflush(stderr);
}

} // namespace nvr
