#pragma once

#include <string>
#include <mutex>
#include <sstream>
#include <iostream>

namespace nvr {

enum class LogLevel {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static Logger& Instance();

    void SetLogLevel(LogLevel level);
    LogLevel GetLogLevel() const;

    void Log(LogLevel level, const char* file, int line, const std::string& message);

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel current_level_{LogLevel::INFO};
    std::mutex mutex_;
};

class LogMessage {
public:
    LogMessage(LogLevel level, const char* file, int line)
        : level_(level), file_(file), line_(line) {}

    ~LogMessage() {
        Logger::Instance().Log(level_, file_, line_, stream_.str());
    }

    template <typename T>
    LogMessage& operator<<(const T& val) {
        stream_ << val;
        return *this;
    }

private:
    LogLevel level_;
    const char* file_;
    int line_;
    std::ostringstream stream_;
};

#define LOG_DEBUG if (nvr::Logger::Instance().GetLogLevel() <= nvr::LogLevel::DEBUG) nvr::LogMessage(nvr::LogLevel::DEBUG, __FILE__, __LINE__)
#define LOG_INFO  if (nvr::Logger::Instance().GetLogLevel() <= nvr::LogLevel::INFO)  nvr::LogMessage(nvr::LogLevel::INFO,  __FILE__, __LINE__)
#define LOG_WARN  if (nvr::Logger::Instance().GetLogLevel() <= nvr::LogLevel::WARN)  nvr::LogMessage(nvr::LogLevel::WARN,  __FILE__, __LINE__)
#define LOG_ERROR if (nvr::Logger::Instance().GetLogLevel() <= nvr::LogLevel::ERROR) nvr::LogMessage(nvr::LogLevel::ERROR, __FILE__, __LINE__)

} // namespace nvr
