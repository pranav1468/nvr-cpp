#include "nvr/ingress/camera_manager.h"
#include "nvr/ingress/onvif_client.h"
#include "nvr/common/logger.h"
#include "nvr/common/deep_tracer.h"
#include "nvr/common/time_utils.h"
#include "nvr/common/crypto_utils.h"
#include "nvr/database/database_manager.h"
#include "nvr/media/stream_broker.h"
#include "nvr/recording/recording_scheduler.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace nvr {

CameraManager& CameraManager::Instance() {
    static CameraManager instance;
    return instance;
}

CameraManager::~CameraManager() {
    StopAll();
}

void CameraManager::Initialize(const std::vector<CameraConfig>& configs) {
    if (!configs.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cameras_.clear();
        for (const auto& cfg : configs) {
            CameraEntry entry;
            entry.config = cfg;
            cameras_[cfg.id] = std::move(entry);

            CameraRecord rec;
            rec.channel_id = cfg.id;
            rec.name = cfg.name;
            rec.rtsp_main_url = cfg.rtsp_url;
            rec.rtsp_sub_url = cfg.sub_rtsp_url;
            rec.onvif_xaddr = cfg.onvif_xaddr;
            rec.onvif_username = cfg.onvif_username;
            rec.onvif_password = cfg.onvif_password;
            rec.enabled = cfg.enabled;
            rec.record_mode = static_cast<int>(cfg.record_mode);
            rec.updated_at = time_utils::WallTimeMs();
            DatabaseManager::Instance().UpsertCamera(rec);
        }
        LOG_INFO << "CameraManager initialized with " << cameras_.size() << " configured cameras from nvr_config.json (synced to SQLite)";
        return;
    }

    if (LoadCamerasFromDb()) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_INFO << "CameraManager initialized with " << cameras_.size() << " cameras from SQLite database";
        return;
    }
}

bool CameraManager::LoadCamerasFromDb() {
    auto db_cams = DatabaseManager::Instance().GetCameras();
    if (db_cams.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cameras_.clear();
    for (const auto& rec : db_cams) {
        CameraConfig cfg;
        cfg.id = rec.channel_id;
        cfg.name = rec.name;
        cfg.rtsp_url = rec.rtsp_main_url;
        cfg.sub_rtsp_url = rec.rtsp_sub_url;
        cfg.onvif_xaddr = rec.onvif_xaddr;
        cfg.onvif_username = rec.onvif_username;
        cfg.onvif_password = rec.onvif_password;
        cfg.enabled = rec.enabled;
        cfg.record_mode = static_cast<RecordMode>(rec.record_mode);
        cfg.reconnect_delay_sec = 3;

        CameraEntry entry;
        entry.config = cfg;
        cameras_[cfg.id] = std::move(entry);
    }
    return true;
}

int CameraManager::FindChannelByRtspUrl(const std::string& rtsp_url, int exclude_channel_id) const {
    if (rtsp_url.empty()) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pair : cameras_) {
        const auto& cam = pair.second.config;
        if (cam.id == exclude_channel_id) continue;
        if (cam.rtsp_url == rtsp_url || (!cam.sub_rtsp_url.empty() && cam.sub_rtsp_url == rtsp_url)) {
            return cam.id;
        }
    }
    return -1;
}

bool CameraManager::AddCamera(const CameraConfig& config) {
    TRACE_SCOPE("INGRESS");
    if (!config.rtsp_url.empty()) {
        int dup_ch = FindChannelByRtspUrl(config.rtsp_url, config.id);
        if (dup_ch != -1) {
            LOG_WARN << "CameraManager: Rejecting AddCamera for Ch " << config.id 
                     << " — RTSP URL '" << config.rtsp_url << "' already assigned to Ch " << dup_ch;
            return false;
        }
    }
    if (!config.sub_rtsp_url.empty()) {
        int dup_ch = FindChannelByRtspUrl(config.sub_rtsp_url, config.id);
        if (dup_ch != -1) {
            LOG_WARN << "CameraManager: Rejecting AddCamera for Ch " << config.id 
                     << " — Sub RTSP URL '" << config.sub_rtsp_url << "' already assigned to Ch " << dup_ch;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cameras_.find(config.id) != cameras_.end()) {
            LOG_WARN << "[Channel " << config.id << "] Camera already exists";
            return false;
        }
        if (cameras_.size() >= max_channels_) {
            LOG_WARN << "Cannot add camera " << config.id << ": maximum channel capacity (" 
                     << max_channels_ << ") reached";
            return false;
        }
        CameraEntry entry;
        entry.config = config;
        cameras_[config.id] = std::move(entry);
    }
    LOG_INFO << "[Channel " << config.id << "] Added camera: " << config.name;
    TRACE_EVENT("INGRESS", "Added camera channel " + std::to_string(config.id) + " (" + config.name + ")");

    // Persist to SQLite and configuration file
    CameraRecord rec;
    rec.channel_id = config.id;
    rec.name = config.name;
    rec.rtsp_main_url = config.rtsp_url;
    rec.rtsp_sub_url = config.sub_rtsp_url;
    rec.onvif_xaddr = config.onvif_xaddr;
    rec.onvif_username = config.onvif_username;
    rec.onvif_password = config.onvif_password;
    rec.enabled = config.enabled;
    rec.record_mode = static_cast<int>(config.record_mode);
    rec.updated_at = time_utils::WallTimeMs();
    DatabaseManager::Instance().UpsertCamera(rec);

    SaveCamerasToFile();
    return true;
}

bool CameraManager::GetCamera(int channel_id, CameraConfig& out_cfg) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end()) return false;
    out_cfg = it->second.config;
    return true;
}

bool CameraManager::UpdateCamera(const CameraConfig& config) {
    TRACE_SCOPE("INGRESS");
    if (!config.rtsp_url.empty()) {
        int dup_ch = FindChannelByRtspUrl(config.rtsp_url, config.id);
        if (dup_ch != -1) {
            LOG_WARN << "CameraManager: Rejecting UpdateCamera for Ch " << config.id 
                     << " — RTSP URL '" << config.rtsp_url << "' already assigned to Ch " << dup_ch;
            return false;
        }
    }
    if (!config.sub_rtsp_url.empty()) {
        int dup_ch = FindChannelByRtspUrl(config.sub_rtsp_url, config.id);
        if (dup_ch != -1) {
            LOG_WARN << "CameraManager: Rejecting UpdateCamera for Ch " << config.id 
                     << " — Sub RTSP URL '" << config.sub_rtsp_url << "' already assigned to Ch " << dup_ch;
            return false;
        }
    }

    std::unique_ptr<StreamSession> old_main, old_sub;
    std::unique_ptr<std::thread> old_thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cameras_.find(config.id);
        if (it == cameras_.end()) {
            if (cameras_.size() >= max_channels_) {
                LOG_WARN << "Cannot upsert camera " << config.id << ": maximum channel capacity (" 
                         << max_channels_ << ") reached";
                return false;
            }
            CameraEntry entry;
            entry.config = config;
            cameras_[config.id] = std::move(entry);
        } else {
            old_main = std::move(it->second.main_session);
            old_sub = std::move(it->second.sub_session);
            if (it->second.event_running) {
                it->second.event_running->store(false);
            }
            if (it->second.event_thread && it->second.event_thread->joinable()) {
                old_thread = std::move(it->second.event_thread);
            }
            it->second.config = config;
        }
    }

    if (old_main) old_main->Stop();
    if (old_sub) old_sub->Stop();
    if (old_thread && old_thread->joinable()) old_thread->join();



    if (config.enabled && !config.rtsp_url.empty()) {
        if (config.record_mode != RecordMode::DISABLED || config.sub_rtsp_url.empty()) {
            StartCamera(config.id);
        }
        if (!config.sub_rtsp_url.empty()) {
            StartSubStream(config.id);
        }
    }
    LOG_INFO << "[Channel " << config.id << "] Updated camera configuration (" << config.name << ")";
    TRACE_EVENT("INGRESS", "Updated camera channel " + std::to_string(config.id) + " (" + config.name + ")");

    // Persist to SQLite
    CameraRecord rec;
    rec.channel_id = config.id;
    rec.name = config.name;
    rec.rtsp_main_url = config.rtsp_url;
    rec.rtsp_sub_url = config.sub_rtsp_url;
    rec.onvif_xaddr = config.onvif_xaddr;
    rec.onvif_username = config.onvif_username;
    rec.onvif_password = config.onvif_password;
    rec.enabled = config.enabled;
    rec.record_mode = static_cast<int>(config.record_mode);
    rec.updated_at = time_utils::WallTimeMs();
    DatabaseManager::Instance().UpsertCamera(rec);

    SaveCamerasToFile();
    return true;
}

bool CameraManager::RemoveCamera(int channel_id) {
    TRACE_SCOPE("INGRESS");
    std::unique_ptr<StreamSession> old_main, old_sub;
    std::unique_ptr<std::thread> old_thread;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cameras_.find(channel_id);
        if (it != cameras_.end()) {
            found = true;
            old_main = std::move(it->second.main_session);
            old_sub = std::move(it->second.sub_session);
            if (it->second.event_running) {
                it->second.event_running->store(false);
            }
            if (it->second.event_thread && it->second.event_thread->joinable()) {
                old_thread = std::move(it->second.event_thread);
            }
            cameras_.erase(it);
        }
    }
    if (!found) {
        return false;
    }

    if (old_main) old_main->Stop();
    if (old_sub) old_sub->Stop();
    if (old_thread && old_thread->joinable()) old_thread->join();

    LOG_INFO << "[Channel " << channel_id << "] Removed camera";
    TRACE_EVENT("INGRESS", "Removed camera channel " + std::to_string(channel_id));

    // Remove from SQLite
    DatabaseManager::Instance().DeleteCamera(channel_id);

    SaveCamerasToFile();
    return true;
}

bool CameraManager::StartCamera(int channel_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cameras_.find(channel_id);
        if (it == cameras_.end()) {
            return false;
        }

        if (!it->second.config.enabled || it->second.config.rtsp_url.empty()) {
            LOG_WARN << "[Channel " << channel_id << "] Camera disabled or RTSP URL empty";
            return false;
        }

        if (!it->second.main_session) {
            it->second.main_session = std::make_unique<StreamSession>(
                channel_id,
                StreamType::MAIN,
                it->second.config.rtsp_url,
                it->second.config.reconnect_delay_sec > 0 ? it->second.config.reconnect_delay_sec : 5
            );
        }

        if (!it->second.main_session->IsConnected()) {
            it->second.main_session->Start();
            TRACE_EVENT("INGRESS", "Started MAIN RTSP session on Ch " + std::to_string(channel_id));
        }
    }

    return true;
}

void CameraManager::StopCamera(int channel_id, bool force) {
    std::unique_ptr<StreamSession> session_to_stop;
    std::unique_ptr<std::thread> thread_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cameras_.find(channel_id);
        if (it == cameras_.end() || !it->second.main_session) {
            return;
        }

        if (!force) {
            if (RecordingScheduler::Instance().IsChannelActive(channel_id)) {
                LOG_INFO << "[CameraManager] Channel " << channel_id
                         << " MAIN stream active in recording; retaining RTSP session";
                return;
            }
            size_t main_count = StreamBroker::Instance().GetSubscriberCount(channel_id, StreamType::MAIN);
            if (main_count > 0) {
                LOG_INFO << "[CameraManager] Channel " << channel_id
                         << " MAIN stream has " << main_count << " active subscribers; retaining RTSP session";
                return;
            }
        }

        session_to_stop = std::move(it->second.main_session);
        if (it->second.event_running) {
            it->second.event_running->store(false);
        }
        if (it->second.event_thread && it->second.event_thread->joinable()) {
            thread_to_join = std::move(it->second.event_thread);
        }
    }

    if (session_to_stop) {
        session_to_stop->Stop();
        TRACE_EVENT("INGRESS", "Stopped MAIN RTSP session on Ch " + std::to_string(channel_id));
    }
    if (thread_to_join && thread_to_join->joinable()) {
        thread_to_join->join();
    }
}

bool CameraManager::StartSubStream(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end()) {
        return false;
    }

    if (!it->second.config.enabled) {
        LOG_WARN << "[Channel " << channel_id << "] Camera disabled; ignoring StartSubStream";
        return false;
    }

    std::string sub_url = it->second.config.sub_rtsp_url;
    if (sub_url.empty()) {
        sub_url = it->second.config.rtsp_url;
    }
    if (sub_url.empty()) {
        LOG_WARN << "[Channel " << channel_id << "] No RTSP URL available for SUB stream";
        return false;
    }

    if (it->second.sub_session && it->second.sub_session->IsConnected()) {
        return true;
    }

    if (!it->second.sub_session) {
        it->second.sub_session = std::make_unique<StreamSession>(
            channel_id,
            StreamType::SUB,
            sub_url,
            it->second.config.reconnect_delay_sec > 0 ? it->second.config.reconnect_delay_sec : 5
        );
    }

    it->second.sub_session->Start();
    TRACE_EVENT("INGRESS", "Started SUB RTSP session on Ch " + std::to_string(channel_id));
    return true;
}

void CameraManager::StopSubStream(int channel_id, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end() || !it->second.sub_session) {
        return;
    }

    if (!force) {
        size_t sub_count = StreamBroker::Instance().GetSubscriberCount(channel_id, StreamType::SUB);
        if (sub_count > 0) {
            LOG_INFO << "[CameraManager] Channel " << channel_id
                     << " SUB stream has " << sub_count << " active subscribers (e.g. AI/Grid); retaining RTSP session";
            return;
        }
    }

    it->second.sub_session->Stop();
    TRACE_EVENT("INGRESS", "Stopped SUB RTSP session on Ch " + std::to_string(channel_id));
}

bool CameraManager::EnsureMainStream(int channel_id) {
    return StartCamera(channel_id);
}


void CameraManager::StopAll() {
    std::vector<std::unique_ptr<StreamSession>> sessions;
    std::vector<std::unique_ptr<std::thread>> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, entry] : cameras_) {
            if (entry.main_session) {
                sessions.push_back(std::move(entry.main_session));
            }
            if (entry.sub_session) {
                sessions.push_back(std::move(entry.sub_session));
            }
            if (entry.event_running) {
                entry.event_running->store(false);
            }
            if (entry.event_thread && entry.event_thread->joinable()) {
                threads.push_back(std::move(entry.event_thread));
            }
        }
    }
    for (auto& s : sessions) {
        if (s) s->Stop();
    }
    for (auto& t : threads) {
        if (t && t->joinable()) t->join();
    }
}

bool CameraManager::IsCameraOnline(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end()) return false;
    bool main_ok = (it->second.main_session && it->second.main_session->IsConnected());
    bool sub_ok = (it->second.sub_session && it->second.sub_session->IsConnected());
    return main_ok || sub_ok;
}

bool CameraManager::IsSubStreamOnline(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    return (it != cameras_.end() && it->second.sub_session && it->second.sub_session->IsConnected());
}

bool CameraManager::HasCamera(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cameras_.find(channel_id) != cameras_.end();
}

std::vector<CameraConfig> CameraManager::GetCameras() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CameraConfig> list;
    list.reserve(cameras_.size());
    for (const auto& [id, entry] : cameras_) {
        list.push_back(entry.config);
    }
    std::sort(list.begin(), list.end(), [](const CameraConfig& a, const CameraConfig& b) {
        return a.id < b.id;
    });
    return list;
}

void CameraManager::SetConfigFilePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_file_path_ = path;
}

void CameraManager::SaveCamerasToFile() {
    std::string path;
    std::vector<CameraConfig> cams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = config_file_path_;
        if (path.empty()) return;
        for (const auto& [id, entry] : cameras_) {
            cams.push_back(entry.config);
        }
    }
    std::sort(cams.begin(), cams.end(), [](const CameraConfig& a, const CameraConfig& b) {
        return a.id < b.id;
    });

    nlohmann::json j = nlohmann::json::object();
    std::ifstream in(path);
    if (in.is_open()) {
        try {
            in >> j;
        } catch (...) {}
        in.close();
    }

    nlohmann::json cam_arr = nlohmann::json::array();
    for (const auto& c : cams) {
        nlohmann::json cj;
        cj["id"] = c.id;
        cj["name"] = c.name;
        cj["rtsp_url"] = c.rtsp_url;
        cj["sub_rtsp_url"] = c.sub_rtsp_url;
        cj["onvif_xaddr"] = c.onvif_xaddr;
        cj["onvif_username"] = c.onvif_username;
        cj["onvif_password"] = c.onvif_password;
        cj["enabled"] = c.enabled;
        cj["record_mode"] = static_cast<int>(c.record_mode);
        cj["reconnect_delay_sec"] = c.reconnect_delay_sec;
        cam_arr.push_back(cj);
    }
    j["cameras"] = cam_arr;

    std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path);
    if (out.is_open()) {
        out << j.dump(2) << std::endl;
        out.close();
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            LOG_WARN << "Failed to atomically rename updated config: " << ec.message();
        } else {
            LOG_INFO << "Persisted " << cams.size() << " cameras to " << path;
        }
    }
}

} // namespace nvr
