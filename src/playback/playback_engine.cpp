#include "nvr/playback/playback_engine.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <ctime>

namespace nvr {

namespace {

time_t ParseIsoToTimeT(const std::string& iso_str) {
    if (iso_str.empty()) return 0;
    struct tm tm{};
    if (strptime(iso_str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        return timegm(&tm);
    }
    return 0;
}

} // namespace

PlaybackEngine& PlaybackEngine::Instance() {
    static PlaybackEngine instance;
    return instance;
}

PlaybackEngine::PlaybackEngine() {
    std::error_code ec;
    recording_root_ = RetentionManager::Instance().GetRecordingPath();
    if (recording_root_.empty()) {
        recording_root_ = "./recordings";
    }
    std::filesystem::create_directories(recording_root_, ec);
    ClipExporter::Instance().Configure(recording_root_ + "/exports");
}

void PlaybackEngine::Initialize(const std::string& recording_root_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_root_path.empty()) {
        recording_root_ = recording_root_path;
    }
    std::error_code ec;
    std::filesystem::create_directories(recording_root_, ec);
    ClipExporter::Instance().Configure(recording_root_ + "/exports");
    LOG_INFO << "[PlaybackEngine] Initialized with storage root: " << recording_root_;
}

std::filesystem::path PlaybackEngine::GetStorageRoot() const {
    std::error_code ec;
    std::string path_str = RetentionManager::Instance().GetRecordingPath();
    if (path_str.empty()) path_str = recording_root_;
    return std::filesystem::weakly_canonical(path_str, ec);
}

std::vector<std::string> PlaybackEngine::GetRecordingDates() {
    std::error_code ec;
    std::filesystem::path root = GetStorageRoot();
    std::vector<std::string> dates;

    if (std::filesystem::exists(root, ec) && std::filesystem::is_directory(root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (entry.is_directory(ec)) {
                std::string fname = entry.path().filename().string();
                if (fname.size() == 8 && std::all_of(fname.begin(), fname.end(), ::isdigit)) {
                    dates.push_back(fname);
                }
            }
        }
    }

    std::sort(dates.begin(), dates.end(), std::greater<std::string>());
    return dates;
}

nlohmann::json PlaybackEngine::QueryRecordings(const nlohmann::json& filter_json) {
    RecordingQueryFilter filter;
    if (filter_json.contains("channel_id")) {
        if (filter_json["channel_id"].is_number()) filter.channel_id = filter_json["channel_id"].get<int>();
        else if (filter_json["channel_id"].is_string()) {
            try { filter.channel_id = std::stoi(filter_json["channel_id"].get<std::string>()); } catch (...) {}
        }
    }
    if (filter_json.contains("date") && filter_json["date"].is_string()) {
        filter.date = filter_json["date"].get<std::string>();
    }
    if (filter_json.contains("trigger_type") && filter_json["trigger_type"].is_string()) {
        filter.trigger_type = filter_json["trigger_type"].get<std::string>();
    }
    if (filter_json.contains("from_time") && filter_json["from_time"].is_string()) {
        filter.from_time = filter_json["from_time"].get<std::string>();
    }
    if (filter_json.contains("to_time") && filter_json["to_time"].is_string()) {
        filter.to_time = filter_json["to_time"].get<std::string>();
    }
    if (filter_json.contains("limit") && filter_json["limit"].is_number()) {
        filter.limit = filter_json["limit"].get<int>();
    }
    return QueryRecordings(filter);
}

nlohmann::json PlaybackEngine::QueryRecordings(const RecordingQueryFilter& filter) {
    std::error_code ec;
    std::filesystem::path root = GetStorageRoot();
    std::vector<nlohmann::json> items;

    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        nlohmann::json empty_res;
        empty_res["segments"] = nlohmann::json::array();
        empty_res["review_items"] = nlohmann::json::array();
        return empty_res;
    }

    std::vector<std::string> target_dates;
    if (!filter.date.empty()) {
        std::string d_clean;
        for (char c : filter.date) if (std::isdigit(c)) d_clean.push_back(c);
        if (!d_clean.empty()) target_dates.push_back(d_clean);
    } else {
        target_dates = GetRecordingDates();
    }

    for (const auto& d_str : target_dates) {
        std::filesystem::path d_path = root / d_str;
        if (!std::filesystem::exists(d_path, ec) || !std::filesystem::is_directory(d_path, ec)) continue;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(d_path, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".mp4") {
                std::string fname = entry.path().stem().string();

                int ch_id = 1;
                std::string date_part = d_str;
                std::string time_part = "";

                if (fname.rfind("cam", 0) == 0) {
                    size_t under = fname.find('_');
                    if (under != std::string::npos) {
                        try {
                            ch_id = std::stoi(fname.substr(3, under - 3));
                        } catch (...) {}
                        size_t next_under = fname.find('_', under + 1);
                        if (next_under != std::string::npos) {
                            date_part = fname.substr(under + 1, next_under - (under + 1));
                            time_part = fname.substr(next_under + 1);
                        } else {
                            time_part = fname.substr(under + 1);
                        }
                    }
                } else {
                    size_t under = fname.find('_');
                    if (under != std::string::npos) {
                        date_part = fname.substr(0, under);
                        time_part = fname.substr(under + 1);
                    }
                }

                if (filter.channel_id > 0 && ch_id != filter.channel_id) {
                    continue;
                }

                std::string trigger_type = "continuous";
                if (fname.find("_motion") != std::string::npos) trigger_type = "motion";
                else if (fname.find("_ai") != std::string::npos) trigger_type = "ai";
                else if (fname.find("_event") != std::string::npos) trigger_type = "event";

                if (!filter.trigger_type.empty() && filter.trigger_type != "all" && filter.trigger_type != trigger_type) {
                    continue;
                }

                uint64_t fsize = entry.file_size(ec);
                double duration_sec = 60.0;
                if (fsize > 0 && fsize < 100000000) {
                    duration_sec = 60.0;
                }

                std::string iso_start = date_part.substr(0, 4) + "-" + date_part.substr(4, 2) + "-" + date_part.substr(6, 2) + "T";
                if (time_part.size() >= 6) {
                    iso_start += time_part.substr(0, 2) + ":" + time_part.substr(2, 2) + ":" + time_part.substr(4, 2) + "Z";
                } else {
                    iso_start += "00:00:00Z";
                }

                struct tm tm_start{};
                time_t t_start = 0;
                if (strptime(iso_start.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm_start)) {
                    t_start = timegm(&tm_start);
                }
                time_t t_end = t_start + static_cast<time_t>(duration_sec);
                struct tm tm_end{};
                gmtime_r(&t_end, &tm_end);
                char end_buf[32]{0};
                strftime(end_buf, sizeof(end_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_end);
                std::string iso_end(end_buf);

                if (!filter.from_time.empty() && time_part.size() >= 4) {
                    std::string f_clean;
                    for (char c : filter.from_time) if (std::isdigit(c)) f_clean.push_back(c);
                    if (!f_clean.empty() && time_part.substr(0, f_clean.size()) < f_clean) continue;
                }
                if (!filter.to_time.empty() && time_part.size() >= 4) {
                    std::string t_clean;
                    for (char c : filter.to_time) if (std::isdigit(c)) t_clean.push_back(c);
                    if (!t_clean.empty() && time_part.substr(0, t_clean.size()) > t_clean) continue;
                }

                auto lock_path = entry.path().parent_path() / (entry.path().stem().string() + ".locked");
                bool is_locked = std::filesystem::exists(lock_path, ec);

                std::filesystem::path rel_path = std::filesystem::relative(entry.path(), root, ec);
                std::string rel_path_str = rel_path.string();

                int64_t seg_id = static_cast<int64_t>(ch_id) * 10000000000LL;
                try {
                    if (date_part.size() >= 8 && time_part.size() >= 6) {
                        seg_id += std::stoll(date_part + time_part);
                    } else {
                        seg_id += std::hash<std::string>{}(rel_path_str) % 10000000000LL;
                    }
                } catch (...) {
                    seg_id += std::hash<std::string>{}(rel_path_str) % 10000000000LL;
                }

                nlohmann::json item;
                item["id"] = seg_id;
                item["channel_id"] = ch_id;
                item["start_time"] = iso_start;
                item["end_time"] = iso_end;
                item["duration_seconds"] = duration_sec;
                item["file_size_bytes"] = fsize;
                item["trigger_type"] = trigger_type;
                item["is_locked"] = is_locked;
                item["created_at"] = iso_start;
                item["file_path"] = rel_path_str;
                item["video_url"] = "/api/v1/recordings/video?path=" + rel_path_str;

                std::filesystem::path thumb_p = entry.path();
                thumb_p.replace_extension(".bmp");
                if (std::filesystem::exists(thumb_p, ec)) {
                    std::string rel_thumb = std::filesystem::relative(thumb_p, root, ec).string();
                    item["thumbnail_url"] = "/api/v1/recordings/thumbnail?path=" + rel_thumb;
                }

                items.push_back(item);
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("start_time", "") < b.value("start_time", "");
    });

    // Frigate-style Review aggregation: merge consecutive chunks into activity blocks
    std::vector<nlohmann::json> review_items;
    std::map<int, std::vector<nlohmann::json>> channel_segments;
    for (const auto& item : items) {
        channel_segments[item.value("channel_id", 1)].push_back(item);
    }

    for (auto& [ch, segs] : channel_segments) {
        if (segs.empty()) continue;
        std::vector<nlohmann::json> block_segs;

        auto flush_review = [&]() {
            if (block_segs.empty()) return;
            nlohmann::json rev;
            rev["id"] = block_segs.front().value("id", 0LL);
            rev["channel_id"] = ch;
            rev["camera_name"] = "Camera " + std::to_string(ch);
            rev["start_time"] = block_segs.front().value("start_time", "");
            rev["end_time"] = block_segs.back().value("end_time", "");

            double total_dur = 0.0;
            uint64_t total_sz = 0;
            bool any_locked = false;
            std::string primary_thumb = "";
            for (const auto& s : block_segs) {
                total_dur += s.value("duration_seconds", 60.0);
                total_sz += s.value("file_size_bytes", 0ULL);
                if (s.value("is_locked", false)) any_locked = true;
                if (primary_thumb.empty() && s.contains("thumbnail_url")) {
                    primary_thumb = s["thumbnail_url"].get<std::string>();
                }
            }

            rev["duration_seconds"] = total_dur;
            rev["file_size_bytes"] = total_sz;
            rev["trigger_type"] = block_segs.front().value("trigger_type", "continuous");
            rev["is_locked"] = any_locked;
            rev["segment_count"] = block_segs.size();
            rev["file_path"] = block_segs.front().value("file_path", "");
            rev["video_url"] = block_segs.front().value("video_url", "");
            if (!primary_thumb.empty()) rev["thumbnail_url"] = primary_thumb;

            std::vector<nlohmann::json> seg_refs;
            for (const auto& s : block_segs) {
                seg_refs.push_back({
                    {"id", s.value("id", 0LL)},
                    {"file_path", s.value("file_path", "")},
                    {"start_time", s.value("start_time", "")},
                    {"duration_seconds", s.value("duration_seconds", 60.0)}
                });
            }
            rev["segments"] = seg_refs;
            review_items.push_back(rev);
            block_segs.clear();
        };

        for (size_t i = 0; i < segs.size(); ++i) {
            const auto& seg = segs[i];
            if (block_segs.empty()) {
                block_segs.push_back(seg);
                continue;
            }

            const auto& prev = block_segs.back();
            bool same_trigger = (seg.value("trigger_type", "") == prev.value("trigger_type", ""));

            time_t t_prev_end = ParseIsoToTimeT(prev.value("end_time", ""));
            time_t t_curr_start = ParseIsoToTimeT(seg.value("start_time", ""));

            bool is_continuous = (t_curr_start >= t_prev_end && (t_curr_start - t_prev_end) <= 15);

            if (block_segs.size() >= 60) {
                flush_review();
                block_segs.push_back(seg);
                continue;
            }

            if (same_trigger && is_continuous) {
                block_segs.push_back(seg);
            } else {
                flush_review();
                block_segs.push_back(seg);
            }
        }
        flush_review();
    }

    std::sort(review_items.begin(), review_items.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("start_time", "") > b.value("start_time", "");
    });

    nlohmann::json resp;
    resp["segments"] = items;
    resp["review_items"] = review_items;
    return resp;
}

bool PlaybackEngine::SetSegmentLock(const std::string& file_path, bool locked) {
    if (file_path.empty()) return false;
    std::string abs_path = GetSegmentLocalPath(file_path);
    if (abs_path.empty()) return false;

    std::error_code ec;
    std::filesystem::path seg_p(abs_path);
    std::filesystem::path lock_p = seg_p.parent_path() / (seg_p.stem().string() + ".locked");

    if (locked) {
        std::ofstream ofs(lock_p.string());
        if (!ofs.is_open()) return false;
        ofs << "LOCKED\n";
        ofs.close();
    } else {
        std::filesystem::remove(lock_p, ec);
    }
    return true;
}

std::string PlaybackEngine::GetSegmentLocalPath(const std::string& relative_path) {
    if (relative_path.empty() || relative_path.find('\0') != std::string::npos ||
        relative_path.find("..") != std::string::npos) {
        return "";
    }

    std::error_code ec;
    std::filesystem::path root_path = GetStorageRoot();
    std::filesystem::path target_path = (root_path / std::filesystem::path(relative_path).relative_path()).lexically_normal();

    std::string root_str = root_path.lexically_normal().string();
    if (!root_str.empty() && root_str.back() != '/') root_str += '/';
    std::string target_str = target_path.string();

    if (target_str.rfind(root_str, 0) != 0 && target_str != root_path.lexically_normal().string()) {
        return "";
    }

    if (!std::filesystem::exists(target_path, ec) || !std::filesystem::is_regular_file(target_path, ec)) {
        return "";
    }

    return target_str;
}

std::string PlaybackEngine::StartExport(int channel_id, int64_t start_time_ms, int64_t end_time_ms,
                                        const std::string& custom_filename) {
    if (end_time_ms <= start_time_ms) {
        end_time_ms = start_time_ms + 900000LL; // default 15 minutes
    }

    // Query segments overlapping the time range
    time_t t_start = static_cast<time_t>(start_time_ms / 1000LL);
    time_t t_end = static_cast<time_t>(end_time_ms / 1000LL);

    struct tm tm_start{};
    struct tm tm_end{};
    gmtime_r(&t_start, &tm_start);
    gmtime_r(&t_end, &tm_end);

    char d_start[16]{0};
    strftime(d_start, sizeof(d_start), "%Y%m%d", &tm_start);

    RecordingQueryFilter filter;
    filter.channel_id = channel_id;
    filter.date = d_start;

    nlohmann::json res = QueryRecordings(filter);
    std::vector<std::string> segment_paths;

    if (res.contains("segments") && res["segments"].is_array()) {
        for (const auto& s : res["segments"]) {
            std::string s_start_iso = s.value("start_time", "");
            std::string s_end_iso = s.value("end_time", "");
            time_t seg_start = ParseIsoToTimeT(s_start_iso);
            time_t seg_end = ParseIsoToTimeT(s_end_iso);

            if (seg_end >= t_start && seg_start <= t_end) {
                std::string rel_path = s.value("file_path", "");
                std::string local_path = GetSegmentLocalPath(rel_path);
                if (!local_path.empty()) {
                    segment_paths.push_back(local_path);
                }
            }
        }
    }

    return ClipExporter::Instance().StartExport(channel_id, start_time_ms, end_time_ms, custom_filename, segment_paths);
}

nlohmann::json PlaybackEngine::GetExportStatus(const std::string& job_id) {
    return ClipExporter::Instance().GetJobStatus(job_id);
}

nlohmann::json PlaybackEngine::ListExportJobs() {
    return ClipExporter::Instance().ListJobs();
}

bool PlaybackEngine::CancelExport(const std::string& job_id) {
    return ClipExporter::Instance().CancelJob(job_id);
}

std::shared_ptr<SeekController> PlaybackEngine::GetOrCreateSeekController(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = seek_controllers_.find(channel_id);
    if (it != seek_controllers_.end()) {
        return it->second;
    }
    auto sc = std::make_shared<SeekController>();
    seek_controllers_[channel_id] = sc;
    return sc;
}

} // namespace nvr
