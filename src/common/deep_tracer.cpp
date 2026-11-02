#include "nvr/common/deep_tracer.h"
#include "nvr/common/time_utils.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/live/live_controller.h"
#include "nvr/storage/retention_manager.h"
#include "3rdparty/nlohmann/json.hpp"

#include <execinfo.h>
#include <cxxabi.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace nvr {

namespace {

std::atomic<int> g_crash_fd{-1};

struct ProcMetrics {
    long rss_kb{0};
    long vm_size_kb{0};
    int num_threads{0};
    int open_fds{0};
};

ProcMetrics ReadProcessMetrics() {
    ProcMetrics pm;
    std::ifstream status_in("/proc/self/status");
    std::string line;
    while (std::getline(status_in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            iss >> pm.rss_kb;
        } else if (line.rfind("VmSize:", 0) == 0) {
            std::istringstream iss(line.substr(7));
            iss >> pm.vm_size_kb;
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream iss(line.substr(8));
            iss >> pm.num_threads;
        }
    }

    DIR* dir = opendir("/proc/self/fd");
    if (dir) {
        int count = 0;
        while (readdir(dir) != nullptr) {
            count++;
        }
        closedir(dir);
        pm.open_fds = (count > 2) ? (count - 2) : 0;
    }
    return pm;
}

size_t SafeIntToDec(int val, char* buf, size_t buf_size) {
    if (buf_size == 0) return 0;
    char tmp[24];
    int idx = 0;
    unsigned int uval = (val < 0) ? static_cast<unsigned int>(-val) : static_cast<unsigned int>(val);
    if (uval == 0) {
        tmp[idx++] = '0';
    } else {
        while (uval > 0 && idx < 20) {
            tmp[idx++] = static_cast<char>('0' + (uval % 10));
            uval /= 10;
        }
    }
    size_t pos = 0;
    if (val < 0 && pos + 1 < buf_size) {
        buf[pos++] = '-';
    }
    while (idx > 0 && pos + 1 < buf_size) {
        buf[pos++] = tmp[--idx];
    }
    buf[pos] = '\0';
    return pos;
}

} // namespace

DeepTracer& DeepTracer::Instance() {
    static DeepTracer instance;
    return instance;
}

DeepTracer::~DeepTracer() {
    Stop();
}

bool DeepTracer::Start(const std::string& log_file_path,
                       const std::string& storage_path,
                       uint32_t monitor_interval_ms,
                       size_t max_log_size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
        return true;
    }

    log_file_path_ = log_file_path;
    storage_path_ = storage_path;
    monitor_interval_ms_ = (monitor_interval_ms < 200) ? 200 : monitor_interval_ms;
    max_log_size_bytes_ = (max_log_size_bytes < 512 * 1024) ? (512 * 1024) : max_log_size_bytes;

    try {
        std::filesystem::path p(log_file_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {}

    std::string crash_path = log_file_path_ + ".crash";
    int cfd = ::open(crash_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    int old_cfd = g_crash_fd.exchange(cfd);
    if (old_cfd >= 0) {
        ::close(old_cfd);
    }

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
              << "[DeepTracer] Sampling Interval: " << monitor_interval_ms_ << " ms | Max Log Size: " << max_log_size_bytes_ << " bytes\n"
              << "================================================================================\n"
              << std::flush;

    InstallCrashHandlers();

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

    sleep_cv_.notify_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    Logger::Instance().SetSink(nullptr);
    Logger::Instance().SetLogLevel(saved_console_level_);

    UninstallCrashHandlers();

    int cfd = g_crash_fd.exchange(-1);
    if (cfd >= 0) {
        ::close(cfd);
    }

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

void DeepTracer::RotateLogIfNeededLocked() {
    if (!log_file_.is_open() || log_file_path_.empty()) {
        return;
    }
    std::streampos pos = log_file_.tellp();
    if (pos > 0 && static_cast<size_t>(pos) >= max_log_size_bytes_) {
        log_file_.flush();
        log_file_.close();
        std::error_code ec;
        std::filesystem::rename(log_file_path_, log_file_path_ + ".1", ec);
        log_file_.open(log_file_path_, std::ios::out | std::ios::trunc);
        if (log_file_.is_open()) {
            log_file_ << "[DeepTracer] Log rotated at "
                      << time_utils::FormatIso8601(time_utils::WallTimeMs()) << "\n";
        }
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
    uint64_t tid = 0;
#ifdef SYS_gettid
    tid = static_cast<uint64_t>(syscall(SYS_gettid));
#else
    tid = static_cast<uint64_t>(pthread_self());
#endif

    int64_t now_us = time_utils::WallTimeMs() * 1000;
    const char* base_file = file ? strrchr(file, '/') : nullptr;
    base_file = (base_file != nullptr) ? (base_file + 1) : (file ? file : "nvr");

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
    breadcrumbs_.push_back(std::move(b));
    if (breadcrumbs_.size() > kMaxBreadcrumbs) {
        breadcrumbs_.pop_front();
    }

    if (running_.load(std::memory_order_relaxed) && log_file_.is_open()) {
        RotateLogIfNeededLocked();
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

std::string DeepTracer::GetDiagnosticsJson(size_t max_count) const {
    nlohmann::json root;
    int64_t now_ms = time_utils::WallTimeMs();
    ProcMetrics pm = ReadProcessMetrics();

    root["timestamp_ms"] = now_ms;
    root["timestamp_iso"] = time_utils::FormatIso8601(now_ms);
    root["running"] = running_.load();
    root["process"] = {
        {"rss_kb", pm.rss_kb},
        {"vm_size_kb", pm.vm_size_kb},
        {"threads", pm.num_threads},
        {"open_fds", pm.open_fds}
    };
    root["process_metrics"] = root["process"];

    auto recs = GetRecentBreadcrumbs(max_count);
    nlohmann::json crumbs = nlohmann::json::array();
    for (const auto& r : recs) {
        const char* lvl = "INFO";
        switch (r.level) {
            case LogLevel::DEBUG: lvl = "DEBUG"; break;
            case LogLevel::INFO:  lvl = "INFO";  break;
            case LogLevel::WARN:  lvl = "WARN";  break;
            case LogLevel::ERROR: lvl = "ERROR"; break;
        }
        nlohmann::json item;
        item["timestamp_us"] = r.timestamp_us;
        item["thread_id"] = r.thread_id;
        item["level"] = lvl;
        item["file"] = r.file;
        item["line"] = r.line;
        item["tag"] = r.tag;
        item["message"] = r.message;
        crumbs.push_back(std::move(item));
    }
    root["breadcrumbs"] = std::move(crumbs);
    return root.dump();
}

void DeepTracer::MonitorWorkerLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> slock(sleep_mutex_);
            sleep_cv_.wait_for(slock, std::chrono::milliseconds(monitor_interval_ms_), [this]() {
                return !running_.load(std::memory_order_relaxed);
            });
        }
        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }
        CollectPipelineDiagnostics();
    }
}

void DeepTracer::CollectPipelineDiagnostics() {
    std::string time_str = time_utils::FormatIso8601(time_utils::WallTimeMs());
    ProcMetrics pm = ReadProcessMetrics();
    std::ostringstream ss;
    ss << "\n--- [PIPELINE HEALTH SNAPSHOT] " << time_str
       << " | RSS=" << (pm.rss_kb / 1024.0) << "MB Threads=" << pm.num_threads
       << " FDs=" << pm.open_fds << " ---\n";

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
    std::string target_storage;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_storage = storage_path_;
    }
    uint64_t free_mb = RetentionManager::Instance().GetFreeDiskSpaceBytes(target_storage) / (1024 * 1024);
    uint64_t total_mb = RetentionManager::Instance().GetTotalDiskSpaceBytes(target_storage) / (1024 * 1024);
    ss << "  [STORAGE] Disk Free: " << free_mb << " MB / " << total_mb << " MB\n";
    if (free_mb < 500 && total_mb > 0) {
        ss << "      [ALERT] Low storage warning! Less than 500 MB remaining.\n";
    }

    ss << "--------------------------------------------------------\n";

    std::string snapshot = ss.str();
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        RotateLogIfNeededLocked();
        log_file_ << snapshot;
        log_file_.flush();
    }
}

void DeepTracer::InstallCrashHandlers() {
    struct sigaction sa{};
    sa.sa_handler = HandleCrashSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &sa, &old_sa_segv_);
    sigaction(SIGABRT, &sa, &old_sa_abrt_);
    sigaction(SIGFPE,  &sa, &old_sa_fpe_);
    sigaction(SIGBUS,  &sa, &old_sa_bus_);
    sigaction(SIGILL,  &sa, &old_sa_ill_);
}

void DeepTracer::UninstallCrashHandlers() {
    sigaction(SIGSEGV, &old_sa_segv_, nullptr);
    sigaction(SIGABRT, &old_sa_abrt_, nullptr);
    sigaction(SIGFPE,  &old_sa_fpe_,  nullptr);
    sigaction(SIGBUS,  &old_sa_bus_,  nullptr);
    sigaction(SIGILL,  &old_sa_ill_,  nullptr);
}

void DeepTracer::HandleCrashSignal(int signum) {
    char header[96] = "[FATAL] Process received fatal signal ";
    size_t pos = 38;
    pos += SafeIntToDec(signum, header + pos, sizeof(header) - pos);
    const char suffix[] = "; dumping stack trace:\n";
    for (size_t i = 0; i < sizeof(suffix) - 1 && pos + 1 < sizeof(header); ++i) {
        header[pos++] = suffix[i];
    }

    ssize_t w1 = ::write(STDERR_FILENO, header, pos);
    (void)w1;

    void* frames[48];
    int nframes = ::backtrace(frames, 48);
    if (nframes > 0) {
        ::backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
    }

    int cfd = g_crash_fd.load();
    if (cfd >= 0) {
        ssize_t w2 = ::write(cfd, header, pos);
        (void)w2;
        if (nframes > 0) {
            ::backtrace_symbols_fd(frames, nframes, cfd);
        }
        ::fsync(cfd);
    }

    ::signal(signum, SIG_DFL);
    ::raise(signum);
    _exit(128 + signum);
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
