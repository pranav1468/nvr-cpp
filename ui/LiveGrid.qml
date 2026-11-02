import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: root

    // Layout state: 1, 4, 6, 8, or custom tile count
    property int currentLayout: 4
    property bool isFullscreen: false
    property int fullscreenChannelId: 1
    property int activeChannelCount: 8
    property var tileMetrics: ({})
    property var tilesByIndex: ({})

    function updateFromGridState(stateObj) {
        if (!stateObj) return;
        if (stateObj.is_fullscreen !== undefined) root.isFullscreen = stateObj.is_fullscreen;
        if (stateObj.fullscreen_channel_id !== undefined && stateObj.fullscreen_channel_id > 0) {
            root.fullscreenChannelId = stateObj.fullscreen_channel_id;
        }
        if (stateObj.layout_count !== undefined && stateObj.layout_count > 0 && !root.isFullscreen) {
            root.currentLayout = stateObj.layout_count;
        }
        if (stateObj.tiles && stateObj.tiles.length !== undefined) {
            var nextMetrics = {};
            var nextByIndex = {};
            for (var i = 0; i < stateObj.tiles.length; ++i) {
                var t = stateObj.tiles[i];
                if (t) {
                    if (t.channel_id !== undefined) {
                        nextMetrics[t.channel_id] = t;
                    }
                    if (t.tile_index !== undefined) {
                        nextByIndex[t.tile_index] = t;
                    }
                }
            }
            root.tileMetrics = nextMetrics;
            root.tilesByIndex = nextByIndex;
        }
    }

    // Signal to C++ Backend
    signal layoutChangeRequested(int layoutMode)
    signal fullscreenChangeRequested(int channelId)
    signal exitFullscreenRequested()

    color: Theme.bgApp

    // Top Navigation / Layout Control Toolbar
    Rectangle {
        id: toolBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 48
        color: Theme.bgSurface
        border.color: Theme.borderSubtle
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 12

            // System Status Label
            RowLayout {
                spacing: 8
                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: Theme.statusOnline
                }
                Text {
                    text: "LIVE SURVEILLANCE MATRIX"
                    color: Theme.textPrimary
                    font.pixelSize: 12
                    font.bold: true
                    font.family: Theme.fontMono
                }
            }

            Item { Layout.fillWidth: true }

            // Layout Switching Buttons (1 / 4 / 6 / 8 Grid)
            RowLayout {
                spacing: 6
                visible: !root.isFullscreen
                Layout.alignment: Qt.AlignVCenter

                Button {
                    text: "1-CAM"
                    highlighted: root.currentLayout === 1
                    onClicked: root.layoutChangeRequested(1)
                }

                Button {
                    text: "4-CAM (2x2)"
                    highlighted: root.currentLayout === 4
                    onClicked: root.layoutChangeRequested(4)
                }

                Button {
                    text: "6-CAM (3x2)"
                    highlighted: root.currentLayout === 6
                    onClicked: root.layoutChangeRequested(6)
                }

                Button {
                    text: "8-CAM (4x2)"
                    highlighted: root.currentLayout === 8
                    onClicked: root.layoutChangeRequested(8)
                }
            }

            // Return from Fullscreen Button
            Button {
                visible: root.isFullscreen
                text: "⤓ EXIT FULLSCREEN"
                onClicked: root.exitFullscreenRequested()
            }
        }
    }

    // Main Live Grid Display Container
    Item {
        id: gridContainer
        anchors.top: toolBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 6

        // 1. Single Camera Fullscreen Mode
        VideoTile {
            id: fullscreenTile
            anchors.fill: parent
            visible: root.isFullscreen
            tileIndex: 0
            channelId: root.fullscreenChannelId
            cameraName: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].camera_name) ? (root.tileMetrics[root.fullscreenChannelId].camera_name + " (Fullscreen)") : ("Camera " + root.fullscreenChannelId + " (Fullscreen)")
            streamType: "MAIN"
            resolutionWidth: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].stream_width) ? root.tileMetrics[root.fullscreenChannelId].stream_width : 0
            resolutionHeight: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].stream_height) ? root.tileMetrics[root.fullscreenChannelId].stream_height : 0
            fps: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].decoded_fps) ? root.tileMetrics[root.fullscreenChannelId].decoded_fps : 0.0
            droppedFrames: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].dropped_frames) ? root.tileMetrics[root.fullscreenChannelId].dropped_frames : 0
            isRecording: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].is_recording) ? root.tileMetrics[root.fullscreenChannelId].is_recording : false
            status: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].status) ? root.tileMetrics[root.fullscreenChannelId].status : "STREAMING"
            frameRevision: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].frame_revision) ? root.tileMetrics[root.fullscreenChannelId].frame_revision : 0
            isOnline: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].is_online !== undefined) ? root.tileMetrics[root.fullscreenChannelId].is_online : true
            hwAccelerated: (root.tileMetrics[root.fullscreenChannelId] && root.tileMetrics[root.fullscreenChannelId].hw_accelerated !== undefined) ? root.tileMetrics[root.fullscreenChannelId].hw_accelerated : false
            isFullscreen: true
            onFullscreenRequested: root.exitFullscreenRequested()
        }

        // 2. Multi-Camera Grid Matrix (1 / 4 / 6 / 8 / Custom Views)
        GridLayout {
            id: multiGrid
            anchors.fill: parent
            visible: !root.isFullscreen
            columnSpacing: 6
            rowSpacing: 6

            columns: root.currentLayout <= 1 ? 1 :
                     root.currentLayout <= 4 ? 2 :
                     root.currentLayout <= 6 ? 3 : 4

            rows: root.currentLayout <= 1 ? 1 :
                  Math.ceil(root.currentLayout / multiGrid.columns)

            Repeater {
                model: root.currentLayout

                VideoTile {
                    id: tile
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    property var tileInfo: root.tilesByIndex[index] ? root.tilesByIndex[index] : root.tileMetrics[index + 1]
                    property int dynChannelId: (tileInfo && tileInfo.channel_id !== undefined) ? tileInfo.channel_id : (index + 1)

                    tileIndex: index
                    channelId: dynChannelId
                    cameraName: (tileInfo && tileInfo.camera_name) ? tileInfo.camera_name : ("Camera " + dynChannelId)
                    streamType: (tileInfo && tileInfo.stream_type) ? tileInfo.stream_type : (root.currentLayout === 1 ? "MAIN" : "SUB")
                    resolutionWidth: (tileInfo && tileInfo.stream_width) ? tileInfo.stream_width : 0
                    resolutionHeight: (tileInfo && tileInfo.stream_height) ? tileInfo.stream_height : 0
                    fps: (tileInfo && tileInfo.decoded_fps) ? tileInfo.decoded_fps : 0.0
                    droppedFrames: (tileInfo && tileInfo.dropped_frames) ? tileInfo.dropped_frames : 0
                    isRecording: (tileInfo && tileInfo.is_recording) ? tileInfo.is_recording : false
                    status: (tileInfo && tileInfo.status) ? tileInfo.status : "STREAMING"
                    frameRevision: (tileInfo && tileInfo.frame_revision) ? tileInfo.frame_revision : 0
                    isOnline: (tileInfo && tileInfo.is_online !== undefined) ? tileInfo.is_online : true
                    hwAccelerated: (tileInfo && tileInfo.hw_accelerated !== undefined) ? tileInfo.hw_accelerated : false
                    isFullscreen: false

                    onFullscreenRequested: function(chId) {
                        root.fullscreenChangeRequested(chId)
                    }
                }
            }
        }
    }
}
