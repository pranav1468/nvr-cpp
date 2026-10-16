#include "nvr/ingress/camera_manager.h"
#include "nvr/common/logger.h"
#include "nvr/media/stream_broker.h"
#include "nvr/recording/recording_scheduler.h"

namespace nvr {

CameraManager& CameraManager::Instance() {
    static CameraManager instance;
    return instance;
}

CameraManager::~CameraManager() {
    StopAll();
}

void CameraManager::Initialize(const std::vector<CameraConfig>& configs) {
    std::lock_guard<std::mutex> lock(mutex_);
    cameras_.clear();
    for (const auto& cfg : configs) {
        CameraEntry entry;
        entry.config = cfg;
        cameras_[cfg.id] = std::move(entry);
    }
    LOG_INFO << "CameraManager initialized with " << cameras_.size() << " configured cameras";
}

bool CameraManager::AddCamera(const CameraConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cameras_.find(config.id) != cameras_.end()) {
        LOG_WARN << "[Channel " << config.id << "] Camera already exists";
        return false;
    }
    CameraEntry entry;
    entry.config = config;
    cameras_[config.id] = std::move(entry);
    LOG_INFO << "[Channel " << config.id << "] Added camera: " << config.name;
    return true;
}

bool CameraManager::RemoveCamera(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it != cameras_.end()) {
        if (it->second.main_session) {
            it->second.main_session->Stop();
        }
        if (it->second.sub_session) {
            it->second.sub_session->Stop();
        }
        cameras_.erase(it);
        LOG_INFO << "[Channel " << channel_id << "] Removed camera";
        return true;
    }
    return false;
}

bool CameraManager::StartCamera(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end()) {
        return false;
    }

    if (!it->second.config.enabled || it->second.config.rtsp_url.empty()) {
        LOG_WARN << "[Channel " << channel_id << "] Camera disabled or RTSP URL empty";
        return false;
    }

    if (it->second.main_session && it->second.main_session->IsConnected()) {
        return true;
    }

    if (!it->second.main_session) {
        it->second.main_session = std::make_unique<StreamSession>(
            channel_id,
            StreamType::MAIN,
            it->second.config.rtsp_url
        );
    }

    it->second.main_session->Start();
    return true;
}

void CameraManager::StopCamera(int channel_id, bool force) {
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

    it->second.main_session->Stop();
}

bool CameraManager::StartSubStream(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    if (it == cameras_.end()) {
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
            sub_url
        );
    }

    it->second.sub_session->Start();
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
}

bool CameraManager::EnsureMainStream(int channel_id) {
    return StartCamera(channel_id);
}

void CameraManager::StartAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, entry] : cameras_) {
        if (entry.config.enabled && !entry.config.rtsp_url.empty()) {
            if (!entry.main_session) {
                entry.main_session = std::make_unique<StreamSession>(
                    id,
                    StreamType::MAIN,
                    entry.config.rtsp_url
                );
            }
            entry.main_session->Start();
        }
    }
}

void CameraManager::StopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, entry] : cameras_) {
        if (entry.main_session) {
            entry.main_session->Stop();
        }
        if (entry.sub_session) {
            entry.sub_session->Stop();
        }
    }
}

bool CameraManager::IsCameraOnline(int channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cameras_.find(channel_id);
    return (it != cameras_.end() && it->second.main_session && it->second.main_session->IsConnected());
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
    for (const auto& [id, entry] : cameras_) {
        list.push_back(entry.config);
    }
    return list;
}

} // namespace nvr
