#pragma once

#include <QObject>
#include "nvr/live/qml_video_bridge.h"

namespace nvr {

class LiveBridgeController : public QObject {
    Q_OBJECT
public:
    explicit LiveBridgeController(QObject* parent = nullptr) : QObject(parent) {}
    ~LiveBridgeController() override = default;

    Q_INVOKABLE void attachTileSurface(int tileIndex, QObject* surfaceItem) {
        QmlVideoBridge::Instance().AttachTileVideoSurface(tileIndex, static_cast<void*>(surfaceItem));
    }

    Q_INVOKABLE void detachTileSurface(int tileIndex) {
        QmlVideoBridge::Instance().DetachTileVideoSurface(tileIndex);
    }
};

} // namespace nvr
