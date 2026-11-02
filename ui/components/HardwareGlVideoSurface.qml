import QtQuick 2.15
import org.freedesktop.gstreamer.Qt6GLVideoItem 1.0

GstGLQt6VideoItem {
    id: glItem
    anchors.fill: parent

    Component.onCompleted: {
        if (typeof LiveBridgeController !== "undefined" && LiveBridgeController.attachTileSurface) {
            LiveBridgeController.attachTileSurface(parent.tileIndex !== undefined ? parent.tileIndex : 0, glItem);
        }
    }

    Component.onDestruction: {
        if (typeof LiveBridgeController !== "undefined" && LiveBridgeController.detachTileSurface) {
            LiveBridgeController.detachTileSurface(parent.tileIndex !== undefined ? parent.tileIndex : 0);
        }
    }
}
