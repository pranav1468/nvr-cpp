#pragma once

#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <chrono>
#include <csignal>

namespace nvr {

struct TraceBreadcrumb {
    int64_t timestamp_us{0};
    uint64_t thread_id{0};
    LogLevel level{LogLevel::INFO};
    std::string file;
    int line{0};
    std::string tag;
    std::string message;
};

class DeepTracer {
public:
    static DeepTracer& Instance();

    // Start deep background monitoring and tracing
    bool Start(const std::string& log_file_path = "logs/nvr_deep_trace.log",
               const std::string& storage_path = "recordings",
               uint32_t monitor_interval_ms = 1000);

    // Stop deep tracing and flush all logs
    void Stop();

    // Configure storage path for disk monitoring
    void SetStoragePath(const std::string& storage_path);

    // Record an explicit breadcrumb event into flight recorder and deep trace log
    void RecordEvent(const std::string& tag, const std::string& message, const char* file, int line);

    // Internal log sink callback hooked into Logger::Instance()
    void OnLog(LogLevel level, const char* file, int line, const std::string& message);

    // Check if tracer is running
    bool IsRunning() const;

    // Get recent flight recorder history
    std::vector<TraceBreadcrumb> GetRecentBreadcrumbs(size_t max_count = 100) const;

    // Dump flight recorder history to stream
    void DumpFlightRecorder(std::ostream& os, size_t max_count = 500) const;

    // Write crash dump with demangled backtrace
    static void HandleCrashSignal(int signum);

private:
    DeepTracer() = default;
    ~DeepTracer();
    DeepTracer(const DeepTracer&) = delete;
    DeepTracer& operator=(const DeepTracer&) = delete;

    void MonitorWorkerLoop();
    void InstallCrashHandlers();
    void UninstallCrashHandlers();
    void AppendTraceLine(const std::string& line, bool flush_now = false);
    void CollectPipelineDiagnostics();

    mutable std::mutex mutex_;
    std::string log_file_path_;
    std::string storage_path_{"recordings"};
    uint32_t monitor_interval_ms_{1000};
    std::ofstream log_file_;

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;

    static constexpr size_t kMaxBreadcrumbs = 2048;
    std::deque<TraceBreadcrumb> breadcrumbs_;

    LogLevel saved_console_level_{LogLevel::INFO};
    struct sigaction old_sa_segv_{};
    struct sigaction old_sa_abrt_{};
    struct sigaction old_sa_fpe_{};
    struct sigaction old_sa_bus_{};
    struct sigaction old_sa_ill_{};
};

// RAII helper for tracking execution time of critical code scopes
class DeepScopeTracer {
public:
    DeepScopeTracer(const std::string& scope_name, const char* file, int line);
    ~DeepScopeTracer();

private:
    std::string scope_name_;
    const char* file_;
    int line_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace nvr

#ifdef NVR_ENABLE_DEEP_MONITORING
#define TRACE_EVENT(tag, msg) nvr::DeepTracer::Instance().RecordEvent(tag, msg, __FILE__, __LINE__)
#define TRACE_SCOPE(name) nvr::DeepScopeTracer _scope_tracer_(name, __FILE__, __LINE__)
#else
#define TRACE_EVENT(tag, msg) ((void)0)
#define TRACE_SCOPE(name) ((void)0)
#endif
