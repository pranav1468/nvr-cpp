#include "nvr/common/deep_tracer.h"
#include "nvr/common/time_utils.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/live/live_controller.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/storage/segment_index.h"

#include <execinfo.h>
#include <cxxabi.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace nvr {

DeepTracer& DeepTracer::Instance() {
    static DeepTracer instance;
    return instance;
}

DeepTracer::~DeepTracer() {
    Stop();
}

bool DeepTracer::Start(const std::string& log_file_path,
                       const std::string& storage_path,
                       uint32_t monitor_interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
        return true;
    }

    log_file_path_ = log_file_path;
    storage_path_ = storage_path;
    monitor_interval_ms_ = monitor_interval_ms;

    // Ensure logs directory exists
    try {
        std::filesystem::path p(log_file_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {}

    log_file_.open(log_file_path_, std::ios::out | std::ios::app);
    if (!log_file_.is_open()) {
        std::fprintf(stderr, "[DeepTracer] Error: Failed to open trace log file: %s\n", log_file_path_.c_str());
        return false;
    }

    std::string now_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
    log_file_ << "\n================================================================================\n"
              << "[DeepTracer] Deep Diagnostics & Telemetry Sentinel Started at " << now_str << "\n"
              << "[DeepTracer] Log Destination: " << log_file_path_ << "\n"
              << "[DeepTracer] Target Storage Path: " << storage_path_ << "\n"
              << "[DeepTracer] Sampling Interval: " << monitor_interval_ms_ << " ms\n"
              << "================================================================================\n"
              << std::flush;

    InstallCrashHandlers();

    // Hook Logger: capture all levels (including DEBUG) into tracer
    saved_console_level_ = Logger::Instance().GetConsoleLogLevel();
    Logger::Instance().SetLogLevel(LogLevel::DEBUG);
    Logger::Instance().SetSink([this](LogLevel level, const char* file, int line, const std::string& msg) {
        OnLog(level, file, line, msg);
    });

    running_.store(true);
    monitor_thread_ = std::thread(&DeepTracer::MonitorWorkerLoop, this);
    return true;
}

void DeepTracer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    Logger::Instance().SetSink(nullptr);
    Logger::Instance().SetLogLevel(saved_console_level_);

    UninstallCrashHandlers();

    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        std::string now_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
        log_file_ << "\n================================================================================\n"
                  << "[DeepTracer] Deep Diagnostics Sentinel Terminated Cleanly at " << now_str << "\n"
                  << "================================================================================\n"
                  << std::flush;
        log_file_.close();
    }
}

void DeepTracer::SetStoragePath(const std::string& storage_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_path_ = storage_path;
}

bool DeepTracer::IsRunning() const {
    return running_.load();
}

void DeepTracer::RecordEvent(const std::string& tag, const std::string& message, const char* file, int line) {
    OnLog(LogLevel::DEBUG, file, line, "[" + tag + "] " + message);
}

void DeepTracer::OnLog(LogLevel level, const char* file, int line, const std::string& message) {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }

    uint64_t tid = 0;
#ifdef SYS_gettid
    tid = static_cast<uint64_t>(syscall(SYS_gettid));
#else
    tid = static_cast<uint64_t>(pthread_self());
#endif

    int64_t now_us = time_utils::WallTimeMs() * 1000;
    const char* base_file = strrchr(file, '/');
    base_file = (base_file != nullptr) ? (base_file + 1) : file;

    const char* lvl_str = "INFO ";
    switch (level) {
        case LogLevel::DEBUG: lvl_str = "DEBUG"; break;
        case LogLevel::INFO:  lvl_str = "INFO "; break;
        case LogLevel::WARN:  lvl_str = "WARN "; break;
        case LogLevel::ERROR: lvl_str = "ERROR"; break;
    }

    std::string tag = base_file;
    if (!message.empty() && message.front() == '[') {
        size_t end_bracket = message.find(']');
        if (end_bracket != std::string::npos && end_bracket > 1) {
            tag = message.substr(1, end_bracket - 1);
        }
    }

    TraceBreadcrumb b;
    b.timestamp_us = now_us;
    b.thread_id = tid;
    b.level = level;
    b.file = base_file;
    b.line = line;
    b.tag = tag;
    b.message = message;

    std::string time_str = time_utils::FormatIso8601(now_us / 1000);
    char entry[1024];
    std::snprintf(entry, sizeof(entry), "[%s.%03d] [TID:%-5lu] [%s] [%s:%d] %s",
                  time_str.c_str(), static_cast<int>((now_us % 1000000) / 1000),
                  static_cast<unsigned long>(tid), lvl_str, base_file, line, message.c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    breadcrumbs_.push_back(b);
    if (breadcrumbs_.size() > kMaxBreadcrumbs) {
        breadcrumbs_.pop_front();
    }

    if (log_file_.is_open()) {
        log_file_ << entry << "\n";
        if (level >= LogLevel::WARN) {
            if (level == LogLevel::ERROR) {
                log_file_ << ">>> FORENSIC EXECUTION TRAIL (Preceding Events Before Error) <<<\n";
                size_t start_idx = (breadcrumbs_.size() > 25) ? (breadcrumbs_.size() - 25) : 0;
                for (size_t i = start_idx; i < breadcrumbs_.size(); ++i) {
                    const auto& prev = breadcrumbs_[i];
                    log_file_ << "  -> [" << prev.file << ":" << prev.line << "] " << prev.message << "\n";
                }
                log_file_ << ">>> END FORENSIC TRAIL <<<\n";
            }
            log_file_.flush();
        }
    }
}

std::vector<TraceBreadcrumb> DeepTracer::GetRecentBreadcrumbs(size_t max_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = std::min(max_count, breadcrumbs_.size());
    std::vector<TraceBreadcrumb> result;
    result.reserve(count);
    size_t start = breadcrumbs_.size() - count;
    for (size_t i = start; i < breadcrumbs_.size(); ++i) {
        result.push_back(breadcrumbs_[i]);
    }
    return result;
}

void DeepTracer::DumpFlightRecorder(std::ostream& os, size_t max_count) const {
    auto recs = GetRecentBreadcrumbs(max_count);
    os << ">>> RECENT FLIGHT RECORDER HISTORY (" << recs.size() << " entries) <<<\n";
    for (const auto& r : recs) {
        os << "  [" << time_utils::FormatIso8601(r.timestamp_us / 1000) << "] [TID " << r.thread_id
           << "] [" << r.file << ":" << r.line << "] " << r.message << "\n";
    }
    os << ">>> END FLIGHT RECORDER DUMP <<<\n";
}

void DeepTracer::MonitorWorkerLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(monitor_interval_ms_));
        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }
        CollectPipelineDiagnostics();
    }
}

void DeepTracer::CollectPipelineDiagnostics() {
    std::string time_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
    std::ostringstream ss;
    ss << "\n--- [PIPELINE HEALTH SNAPSHOT] " << time_str << " ---\n";

    // 1. Ingress Subsystem
    auto cameras = CameraManager::Instance().GetCameras();
    ss << "  [INGRESS] Cameras Configured: " << cameras.size() << "\n";
    for (const auto& cam : cameras) {
        if (!cam.enabled) continue;
        bool main_on = CameraManager::Instance().IsCameraOnline(cam.id);
        bool sub_on = CameraManager::Instance().IsSubStreamOnline(cam.id);
        ss << "    * Cam " << cam.id << " (" << cam.name << "): MainOnline="
           << (main_on ? "YES" : "NO") << ", SubOnline=" << (sub_on ? "YES" : "NO") << "\n";
        if (!main_on && !sub_on) {
            ss << "      [ALERT] Cam " << cam.id << " is currently OFFLINE\n";
        }
    }

    // 2. Recording Subsystem
    ss << "  [RECORDING] Channel Status:\n";
    for (const auto& cam : cameras) {
        if (!cam.enabled) continue;
        bool rec = RecordingScheduler::Instance().IsChannelRecording(cam.id);
        uint32_t frames = RecordingScheduler::Instance().GetChannelFrameCount(cam.id);
        uint64_t bytes = RecordingScheduler::Instance().GetChannelBytesWritten(cam.id);
        uint64_t drops = RecordingScheduler::Instance().GetChannelDroppedPackets(cam.id);
        uint64_t preroll = RecordingScheduler::Instance().GetChannelPrerollPacketCount(cam.id);
        ss << "    * Ch " << cam.id << ": Active=" << (rec ? "YES" : "STANDBY")
           << ", Frames=" << frames << ", Size=" << (bytes / 1024) << " KB"
           << ", Drops=" << drops << ", Preroll=" << preroll << "\n";
        if (drops > 0) {
            ss << "      [ALERT] Ch " << cam.id << " has " << drops << " dropped packets!\n";
        }
    }

    // 3. Live View Subsystem
    auto live_metrics = LiveController::Instance().GetTileMetrics();
    ss << "  [LIVE VIEW] Tiles Active: " << live_metrics.size()
       << ", Layout=" << static_cast<int>(LiveController::Instance().GetCurrentLayout()) << "\n";
    for (const auto& m : live_metrics) {
        ss << "    * Tile " << m.tile_index << " (Ch " << m.channel_id << "): Enq="
           << m.enqueued_frames << ", Rend=" << m.rendered_frames
           << ", Drop=" << m.dropped_frames << ", FPS=" << m.estimated_fps << "\n";
        if (m.dropped_frames > 0) {
            ss << "      [ALERT] Tile " << m.tile_index << " has dropped " << m.dropped_frames << " frames!\n";
        }
    }

    // 4. Storage & Index Subsystem
    uint64_t free_mb = RetentionManager::Instance().GetFreeDiskSpaceBytes(storage_path_) / (1024 * 1024);
    uint64_t total_mb = RetentionManager::Instance().GetTotalDiskSpaceBytes(storage_path_) / (1024 * 1024);
    int seg_count = SegmentIndex::Instance().GetTotalSegmentCount();
    uint64_t stored_mb = SegmentIndex::Instance().GetTotalRecordingSizeBytes() / (1024 * 1024);
    ss << "  [STORAGE] Disk Free: " << free_mb << " MB / " << total_mb << " MB"
       << ", Total Segments: " << seg_count << " (" << stored_mb << " MB stored)\n";
    if (free_mb < 500 && total_mb > 0) {
        ss << "      [ALERT] Low storage warning! Less than 500 MB remaining.\n";
    }

    ss << "--------------------------------------------------------\n";

    std::string snapshot = ss.str();
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        log_file_ << snapshot;
        log_file_.flush();
    }
}

void DeepTracer::InstallCrashHandlers() {
    std::signal(SIGSEGV, HandleCrashSignal);
    std::signal(SIGABRT, HandleCrashSignal);
    std::signal(SIGFPE,  HandleCrashSignal);
    std::signal(SIGBUS,  HandleCrashSignal);
    std::signal(SIGILL,  HandleCrashSignal);
}

void DeepTracer::UninstallCrashHandlers() {
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGABRT, SIG_DFL);
    std::signal(SIGFPE,  SIG_DFL);
    std::signal(SIGBUS,  SIG_DFL);
    std::signal(SIGILL,  SIG_DFL);
}

void DeepTracer::HandleCrashSignal(int signum) {
    const char* sig_name = "UNKNOWN";
    if (signum == SIGSEGV) sig_name = "SIGSEGV (Segmentation Fault)";
    else if (signum == SIGABRT) sig_name = "SIGABRT (Abort)";
    else if (signum == SIGFPE) sig_name = "SIGFPE (Arithmetic Exception)";
    else if (signum == SIGBUS) sig_name = "SIGBUS (Bus Error)";
    else if (signum == SIGILL) sig_name = "SIGILL (Illegal Instruction)";

    char header[512];
    std::snprintf(header, sizeof(header),
                  "\n================================================================================\n"
                  "FATAL CRASH INTERCEPTED: Signal %d (%s)\n"
                  "Timestamp: %s\n"
                  "================================================================================\n"
                  "STACK BACKTRACE:\n",
                  signum, sig_name, time_utils::FormatIso8601(time_utils::WallTimeMs()).c_str());

    std::fprintf(stderr, "%s", header);

    FILE* crash_f = std::fopen("logs/nvr_crash_trace.log", "a");
    if (crash_f) {
        std::fprintf(crash_f, "%s", header);
    }

    void* callstack[64];
    int frames = backtrace(callstack, 64);
    char** symbols = backtrace_symbols(callstack, frames);

    if (symbols) {
        for (int i = 0; i < frames; ++i) {
            char* mangled_name = nullptr;
            char* offset_begin = nullptr;
            char* offset_end = nullptr;

            for (char* p = symbols[i]; *p; ++p) {
                if (*p == '(') mangled_name = p;
                else if (*p == '+') offset_begin = p;
                else if (*p == ')' && offset_begin) {
                    offset_end = p;
                    break;
                }
            }

            char line_buf[1024];
            if (mangled_name && offset_begin && offset_end && mangled_name < offset_begin) {
                *mangled_name++ = '\0';
                *offset_begin++ = '\0';
                *offset_end++ = '\0';

                int status = 0;
                char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
                std::snprintf(line_buf, sizeof(line_buf), "  #%-2d %s: %s +%s %s\n",
                              i, symbols[i], (status == 0 && demangled ? demangled : mangled_name),
                              offset_begin, offset_end);
                if (demangled) free(demangled);
            } else {
                std::snprintf(line_buf, sizeof(line_buf), "  #%-2d %s\n", i, symbols[i]);
            }

            std::fprintf(stderr, "%s", line_buf);
            if (crash_f) std::fprintf(crash_f, "%s", line_buf);
        }
        free(symbols);
    }

    if (crash_f) {
        std::fprintf(crash_f, "================================================================================\n\n");
        std::fflush(crash_f);
        std::fclose(crash_f);
    }

    std::signal(signum, SIG_DFL);
    std::raise(signum);
}

DeepScopeTracer::DeepScopeTracer(const std::string& scope_name, const char* file, int line)
    : scope_name_(scope_name), file_(file), line_(line), start_time_(std::chrono::steady_clock::now()) {
    DeepTracer::Instance().RecordEvent("SCOPE_ENTER", "Entering scope: " + scope_name_, file_, line_);
}

DeepScopeTracer::~DeepScopeTracer() {
    auto end_time = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time_).count();
    std::ostringstream ss;
    ss << "Exiting scope: " << scope_name_ << " (duration: " << std::fixed << std::setprecision(2) << elapsed_ms << " ms)";
    DeepTracer::Instance().RecordEvent("SCOPE_EXIT", ss.str(), file_, line_);
}

} // namespace nvr
