#include "nvr/ui/nvr_bridge.h"
#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/playback/playback_engine.h"
#include "nvr/common/logger.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <QUrl>

namespace nvr {

NvrBridge::NvrBridge(QObject* parent) : QObject(parent) {}

QString NvrBridge::dispatch(const QString& action, const QString& payloadJson) {
    CommandEnvelope env;
    env.action = action.toStdString();
    env.client_id = "local_qml_ui";
    env.source = CommandSource::UI_QML;
    if (!authToken_.isEmpty()) {
        env.auth_token = authToken_.toStdString();
    }
    
    std::string pStr = payloadJson.toStdString();
    if (!pStr.empty() && pStr != "{}") {
        try {
            env.payload = nlohmann::json::parse(pStr);
        } catch (const std::exception& e) {
            LOG_WARN << "NvrBridge: Failed to parse payload JSON for action " << env.action << ": " << e.what();
            env.payload = nlohmann::json::object();
        }
    } else {
        env.payload = nlohmann::json::object();
    }

    CommandResult res = CommandDispatcher::Instance().Dispatch(env);
    return QString::fromStdString(res.ToJson().dump());
}

QString NvrBridge::getLocalFilePath(const QString& pathOrId) {
    if (pathOrId.isEmpty()) return "";
    std::string resolved = PlaybackEngine::Instance().GetSegmentLocalPath(pathOrId.toStdString());
    if (resolved.empty()) {
        std::string raw = pathOrId.toStdString();
        if (std::filesystem::exists(raw)) {
            return pathOrId;
        }
        return "";
    }
    return QString::fromStdString(resolved);
}

QString NvrBridge::getPlaybackUrl(const QString& pathOrId) {
    QString local = getLocalFilePath(pathOrId);
    if (local.isEmpty()) return "";
    if (local.startsWith("file://")) return local;
    return QUrl::fromLocalFile(local).toString();
}

QString NvrBridge::queryRecordingDates() {
    auto dates = PlaybackEngine::Instance().GetRecordingDates();
    nlohmann::json j = dates;
    return QString::fromStdString(j.dump());
}

QString NvrBridge::queryRecordings(int channelId, const QString& dateStr, const QString& type) {
    nlohmann::json filter;
    if (channelId > 0) filter["channel_id"] = channelId;
    if (!dateStr.isEmpty()) filter["date"] = dateStr.toStdString();
    if (!type.isEmpty() && type != "all") filter["trigger_type"] = type.toStdString();
    filter["limit"] = 500;

    auto result = PlaybackEngine::Instance().QueryRecordings(filter);
    return QString::fromStdString(result.dump());
}

bool NvrBridge::setSegmentLock(const QString& pathOrId, bool locked) {
    return PlaybackEngine::Instance().SetSegmentLock(pathOrId.toStdString(), locked);
}

QString NvrBridge::exportClip(int channelId, qint64 startUtcMs, qint64 endUtcMs, const QString& filename) {
    std::string jobId = PlaybackEngine::Instance().StartExport(channelId, static_cast<int64_t>(startUtcMs), static_cast<int64_t>(endUtcMs), filename.toStdString());
    nlohmann::json d;
    d["job_id"] = jobId;
    d["channel_id"] = channelId;
    d["state"] = "RUNNING";
    return QString::fromStdString(d.dump());
}

QString NvrBridge::getExportStatus(const QString& jobId) {
    auto status = PlaybackEngine::Instance().GetExportStatus(jobId.toStdString());
    return QString::fromStdString(status.dump());
}

QString NvrBridge::listExportJobs() {
    auto list = PlaybackEngine::Instance().ListExportJobs();
    return QString::fromStdString(list.dump());
}

bool NvrBridge::cancelExportJob(const QString& jobId) {
    return PlaybackEngine::Instance().CancelExport(jobId.toStdString());
}

} // namespace nvr
