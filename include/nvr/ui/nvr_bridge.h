#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <cstdint>

namespace nvr {

/**
 * NvrBridge: Native in-process QObject bridge for the local Qt/QML UI.
 *
 * Routes local UI requests directly into CommandDispatcher and PlaybackEngine,
 * eliminating loopback HTTP network roundtrips, JSON serialization overhead,
 * and socket buffer latency.
 */
class NvrBridge : public QObject {
    Q_OBJECT
public:
    explicit NvrBridge(QObject* parent = nullptr);
    ~NvrBridge() override = default;

    // Indicates this is an in-process local client (not remote web browser)
    Q_INVOKABLE bool isLocal() const { return true; }

    // Unified dispatch: execute any command envelope through CommandDispatcher
    Q_INVOKABLE QString dispatch(const QString& action, const QString& payloadJson = "{}");

    // Local filesystem path resolution (for direct zero-copy file access)
    Q_INVOKABLE QString getLocalFilePath(const QString& pathOrId);

    // Resolves a direct file:/// URL for Qt Multimedia MediaPlayer
    Q_INVOKABLE QString getPlaybackUrl(const QString& pathOrId);

    // Playback domain query helpers
    Q_INVOKABLE QString queryRecordingDates();
    Q_INVOKABLE QString queryRecordings(int channelId, const QString& dateStr, const QString& type = "all");
    Q_INVOKABLE bool setSegmentLock(const QString& pathOrId, bool locked);

    // Clip export operations
    Q_INVOKABLE QString exportClip(int channelId, qint64 startUtcMs, qint64 endUtcMs, const QString& filename = "");
    Q_INVOKABLE QString getExportStatus(const QString& jobId);
    Q_INVOKABLE QString listExportJobs();
    Q_INVOKABLE bool cancelExportJob(const QString& jobId);
};

} // namespace nvr
