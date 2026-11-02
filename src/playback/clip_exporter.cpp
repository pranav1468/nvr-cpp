#include "nvr/playback/clip_exporter.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace nvr {

namespace {

inline uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void WriteU32BE(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

inline uint64_t ReadU64BE(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadU32BE(p)) << 32) |
           static_cast<uint64_t>(ReadU32BE(p + 4));
}

inline void WriteU64BE(uint8_t* p, uint64_t val) {
    WriteU32BE(p, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
    WriteU32BE(p + 4, static_cast<uint32_t>(val & 0xFFFFFFFF));
}

bool WriteAllBytes(int fd, const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, ptr + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

std::string GenerateJobId(int channel_id) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream oss;
    oss << "exp_ch" << channel_id << "_" << ms;
    return oss.str();
}

} // namespace

ClipExporter& ClipExporter::Instance() {
    static ClipExporter instance;
    return instance;
}

ClipExporter::ClipExporter() {
    std::error_code ec;
    std::filesystem::create_directories(export_dir_, ec);
}

ClipExporter::~ClipExporter() {
    shutting_down_ = true;
}

void ClipExporter::Configure(const std::string& export_directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    export_dir_ = export_directory;
    std::error_code ec;
    std::filesystem::create_directories(export_dir_, ec);
}

std::string ClipExporter::StartExport(int channel_id, int64_t start_time_ms, int64_t end_time_ms,
                                      const std::string& custom_filename,
                                      const std::vector<std::string>& segment_files) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string job_id = GenerateJobId(channel_id);
    std::string filename = custom_filename;
    if (filename.empty()) {
        std::string s_start = time_utils::FormatTimestampCompact(start_time_ms);
        std::string s_end = time_utils::FormatTimestampCompact(end_time_ms);
        filename = "incident_ch" + std::to_string(channel_id) + "_" + s_start + "_" + s_end + ".mp4";
    }
    if (filename.rfind(".mp4") != filename.size() - 4) {
        filename += ".mp4";
    }

    std::filesystem::path out_p = std::filesystem::path(export_dir_) / filename;

    ExportJob job;
    job.job_id = job_id;
    job.channel_id = channel_id;
    job.start_time_ms = start_time_ms;
    job.end_time_ms = end_time_ms;
    job.output_filename = filename;
    job.output_path = out_p.string();
    job.state = ExportState::RUNNING;
    job.progress_percent = 0;
    job.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    cancel_flags_[job_id] = cancel_flag;
    jobs_[job_id] = job;

    std::thread([this, job, segment_files]() {
        this->ExecuteJob(job, segment_files);
    }).detach();

    return job_id;
}

void ClipExporter::ExecuteJob(ExportJob job, std::vector<std::string> segments) {
    LOG_INFO << "[ClipExporter] Starting export job " << job.job_id << " (" << segments.size() << " segments)";

    std::shared_ptr<std::atomic<bool>> cancel_flag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cancel_flags_.find(job.job_id);
        if (it != cancel_flags_.end()) {
            cancel_flag = it->second;
        }
    }
    if (!cancel_flag) {
        cancel_flag = std::make_shared<std::atomic<bool>>(false);
    }

    std::atomic<int> progress{0};
    std::string error_msg;

    // Monitor progress periodically
    std::thread progress_thread([this, &job, &progress, cancel_flag]() {
        while (!cancel_flag->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int p = progress.load();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = jobs_.find(job.job_id);
                if (it != jobs_.end()) {
                    if (it->second.state == ExportState::COMPLETED || it->second.state == ExportState::FAILED) {
                        break;
                    }
                    it->second.progress_percent = p;
                } else {
                    break;
                }
            }
            if (p >= 100) break;
        }
    });

    bool success = StitchMp4Segments(segments, job.output_path, progress, *cancel_flag, error_msg);
    if (progress_thread.joinable()) {
        progress_thread.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(job.job_id);
    if (it != jobs_.end()) {
        if (cancel_flag->load()) {
            it->second.state = ExportState::CANCELLED;
            it->second.error_message = "Export job cancelled by user";
        } else if (success) {
            it->second.state = ExportState::COMPLETED;
            it->second.progress_percent = 100;
            std::error_code ec;
            it->second.output_file_size = std::filesystem::file_size(job.output_path, ec);
            LOG_INFO << "[ClipExporter] Job " << job.job_id << " completed successfully ("
                     << it->second.output_file_size << " bytes): " << job.output_path;
        } else {
            it->second.state = ExportState::FAILED;
            it->second.error_message = error_msg;
            LOG_ERROR << "[ClipExporter] Job " << job.job_id << " failed: " << error_msg;
        }
    }
}

bool ClipExporter::StitchMp4Segments(const std::vector<std::string>& input_files,
                                     const std::string& output_path,
                                     std::atomic<int>& progress_percent,
                                     std::atomic<bool>& cancel_flag,
                                     std::string& out_error) {
    if (input_files.empty()) {
        out_error = "No recording segment files provided for export";
        return false;
    }

    std::string tmp_output = output_path + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp_output, ec);

    // Fast path: Single segment export
    if (input_files.size() == 1) {
        if (!std::filesystem::exists(input_files[0], ec)) {
            out_error = "Input segment file does not exist: " + input_files[0];
            return false;
        }
        std::filesystem::copy_file(input_files[0], tmp_output, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_error = "Failed to copy single segment: " + ec.message();
            return false;
        }
        std::filesystem::rename(tmp_output, output_path, ec);
        progress_percent = 100;
        return true;
    }

    int out_fd = open(tmp_output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) {
        out_error = "Failed to create temporary export file: " + tmp_output;
        return false;
    }

    bool has_written_init = false;
    uint32_t current_seq = 1;
    uint64_t cumulative_decode_time = 0;
    std::vector<uint8_t> io_buf(65536);

    for (size_t file_idx = 0; file_idx < input_files.size(); ++file_idx) {
        if (cancel_flag.load()) {
            close(out_fd);
            std::filesystem::remove(tmp_output, ec);
            out_error = "Export cancelled";
            return false;
        }

        const auto& in_path = input_files[file_idx];
        std::ifstream in_file(in_path, std::ios::binary);
        if (!in_file.is_open()) {
            LOG_WARN << "[ClipExporter] Skipping missing segment: " << in_path;
            continue;
        }

        in_file.seekg(0, std::ios::end);
        uint64_t file_size = static_cast<uint64_t>(in_file.tellg());
        in_file.seekg(0, std::ios::beg);

        uint64_t file_max_duration = 0;

        while (in_file.good() && static_cast<uint64_t>(in_file.tellg()) < file_size) {
            uint64_t box_start = static_cast<uint64_t>(in_file.tellg());
            if (box_start + 8 > file_size) break;

            uint8_t hdr[8];
            in_file.read(reinterpret_cast<char*>(hdr), 8);
            if (in_file.gcount() < 8) break;

            uint32_t s32 = ReadU32BE(hdr);
            char box_type[5]{0};
            std::memcpy(box_type, hdr + 4, 4);

            uint64_t box_size = s32;
            size_t header_len = 8;
            if (s32 == 1) {
                if (box_start + 16 > file_size) break;
                uint8_t large[8];
                in_file.read(reinterpret_cast<char*>(large), 8);
                box_size = ReadU64BE(large);
                header_len = 16;
            } else if (s32 == 0) {
                box_size = file_size - box_start;
            }

            if (box_size < header_len || box_start + box_size > file_size) {
                break;
            }

            std::string type_str(box_type, 4);

            if (type_str == "ftyp" || type_str == "moov") {
                if (!has_written_init) {
                    // Write ftyp and moov from the first valid segment
                    in_file.seekg(box_start, std::ios::beg);
                    std::vector<uint8_t> init_buf(box_size);
                    in_file.read(reinterpret_cast<char*>(init_buf.data()), box_size);
                    WriteAllBytes(out_fd, init_buf.data(), init_buf.size());
                    if (type_str == "moov") {
                        has_written_init = true;
                    }
                } else {
                    // Skip subsequent ftyp and moov
                    in_file.seekg(box_start + box_size, std::ios::beg);
                }
            } else if (type_str == "moof") {
                // Read moof into memory to rewrite sequence number and base decode time
                in_file.seekg(box_start, std::ios::beg);
                std::vector<uint8_t> moof_buf(box_size);
                in_file.read(reinterpret_cast<char*>(moof_buf.data()), box_size);

                // 1. Rewrite mfhd sequence number
                for (size_t i = 8; i + 16 <= moof_buf.size(); ++i) {
                    if (std::memcmp(&moof_buf[i], "mfhd", 4) == 0) {
                        WriteU32BE(&moof_buf[i + 8], current_seq++);
                        break;
                    }
                }

                // 2. Adjust tfdt base media decode time
                for (size_t i = 8; i + 16 <= moof_buf.size(); ++i) {
                    if (std::memcmp(&moof_buf[i], "tfdt", 4) == 0) {
                        uint8_t ver = moof_buf[i + 4];
                        if (ver == 0 && i + 12 <= moof_buf.size()) {
                            uint32_t orig_val = ReadU32BE(&moof_buf[i + 8]);
                            uint32_t adjusted = orig_val + static_cast<uint32_t>(cumulative_decode_time);
                            WriteU32BE(&moof_buf[i + 8], adjusted);
                        } else if (ver == 1 && i + 16 <= moof_buf.size()) {
                            uint64_t orig_val = ReadU64BE(&moof_buf[i + 8]);
                            uint64_t adjusted = orig_val + cumulative_decode_time;
                            WriteU64BE(&moof_buf[i + 8], adjusted);
                        }
                        break;
                    }
                }

                // 3. Scan trun sample durations to advance cumulative decode time
                for (size_t i = 8; i + 20 <= moof_buf.size(); ++i) {
                    if (std::memcmp(&moof_buf[i], "trun", 4) == 0) {
                        uint32_t flags = ReadU32BE(&moof_buf[i + 4]) & 0x00FFFFFF;
                        uint32_t sample_count = ReadU32BE(&moof_buf[i + 8]);
                        size_t sample_offset = i + 12;
                        if (flags & 0x000001) sample_offset += 4; // data_offset
                        if (flags & 0x000004) sample_offset += 4; // first_sample_flags

                        bool has_duration = (flags & 0x000100) != 0;
                        bool has_size = (flags & 0x000200) != 0;
                        bool has_flags = (flags & 0x000400) != 0;
                        bool has_cto = (flags & 0x000800) != 0;

                        size_t entry_size = (has_duration ? 4 : 0) + (has_size ? 4 : 0) +
                                            (has_flags ? 4 : 0) + (has_cto ? 4 : 0);

                        if (entry_size > 0 && sample_offset + (sample_count * entry_size) <= moof_buf.size()) {
                            for (uint32_t s = 0; s < sample_count; ++s) {
                                size_t ep = sample_offset + (s * entry_size);
                                if (has_duration) {
                                    file_max_duration += ReadU32BE(&moof_buf[ep]);
                                } else {
                                    file_max_duration += 3000; // default timescale 90000 / 30fps
                                }
                            }
                        }
                        break;
                    }
                }

                WriteAllBytes(out_fd, moof_buf.data(), moof_buf.size());
            } else if (type_str == "mdat") {
                // Stream mdat payload cleanly
                in_file.seekg(box_start, std::ios::beg);
                uint64_t rem = box_size;
                while (rem > 0 && in_file.good()) {
                    size_t to_read = std::min<uint64_t>(rem, io_buf.size());
                    in_file.read(reinterpret_cast<char*>(io_buf.data()), to_read);
                    size_t r = static_cast<size_t>(in_file.gcount());
                    if (r == 0) break;
                    WriteAllBytes(out_fd, io_buf.data(), r);
                    rem -= r;
                }
            } else {
                // Skip unknown boxes
                in_file.seekg(box_start + box_size, std::ios::beg);
            }
        }

        cumulative_decode_time += (file_max_duration > 0 ? file_max_duration : 5400000ULL); // ~60 sec @ 90kHz
        progress_percent = static_cast<int>((file_idx + 1) * 100 / input_files.size());
    }

    fsync(out_fd);
    close(out_fd);

    if (!has_written_init) {
        std::filesystem::remove(tmp_output, ec);
        out_error = "Invalid recording segments: No valid MP4 moov metadata found";
        return false;
    }

    std::filesystem::rename(tmp_output, output_path, ec);
    if (ec) {
        out_error = "Failed to finalize export file: " + ec.message();
        return false;
    }

    progress_percent = 100;
    return true;
}

nlohmann::json ClipExporter::GetJobStatus(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        nlohmann::json err;
        err["error"] = "Job not found";
        err["job_id"] = job_id;
        return err;
    }

    const auto& job = it->second;
    nlohmann::json res;
    res["job_id"] = job.job_id;
    res["channel_id"] = job.channel_id;
    res["state"] = ExportStateToString(job.state);
    res["progress_percent"] = job.progress_percent;
    res["output_filename"] = job.output_filename;
    res["output_path"] = job.output_path;
    res["output_file_size"] = job.output_file_size;
    res["error_message"] = job.error_message;
    res["created_at_ms"] = job.created_at_ms;
    return res;
}

nlohmann::json ClipExporter::ListJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& [id, job] : jobs_) {
        nlohmann::json item;
        item["job_id"] = job.job_id;
        item["channel_id"] = job.channel_id;
        item["state"] = ExportStateToString(job.state);
        item["progress_percent"] = job.progress_percent;
        item["output_filename"] = job.output_filename;
        item["output_file_size"] = job.output_file_size;
        item["created_at_ms"] = job.created_at_ms;
        list.push_back(item);
    }
    return list;
}

bool ClipExporter::CancelJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cancel_flags_.find(job_id);
    if (it != cancel_flags_.end()) {
        it->second->store(true);
        return true;
    }
    return false;
}

} // namespace nvr
