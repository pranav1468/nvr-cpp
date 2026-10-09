import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: root

    // Layout state: 1, 4, 6, 8, or 100 (fullscreen)
    property int currentLayout: 4
    property bool isFullscreen: false
    property int fullscreenChannelId: 1
    property int activeChannelCount: 8

    // Signal to C++ Backend
    signal layoutChangeRequested(int layoutMode)
    signal fullscreenChangeRequested(int channelId)
    signal exitFullscreenRequested()

    color: "#030712"

    // Top Navigation / Layout Control Toolbar
    Rectangle {
        id: toolBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 48
        color: "#0b1329"
        border.color: "#1e293b"
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
                    color: "#22c55e"
                }
                Text {
                    text: "LIVE SURVEILLANCE MATRIX"
                    color: "#f8fafc"
                    font.pixelSize: 12
                    font.bold: true
                    font.family: "Monospace"
                }
            }

            Item { Layout.fillWidth: true }

            // Layout Switching Buttons (1 / 4 / 6 / 8 Grid)
            RowLayout {
                spacing: 6
                visible: !root.isFullscreen

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
            cameraName: "Camera " + root.fullscreenChannelId + " (Fullscreen)"
            streamType: "MAIN"
            resolutionWidth: 1920
            resolutionHeight: 1080
            isFullscreen: true
            onFullscreenRequested: root.exitFullscreenRequested()
        }

        // 2. Multi-Camera Grid Matrix (1 / 4 / 6 / 8 Views)
        GridLayout {
            id: multiGrid
            anchors.fill: parent
            visible: !root.isFullscreen
            columnSpacing: 6
            rowSpacing: 6

            columns: root.currentLayout === 1 ? 1 :
                     root.currentLayout === 4 ? 2 :
                     root.currentLayout === 6 ? 3 : 4

            rows: root.currentLayout === 1 ? 1 : 2

            Repeater {
                model: root.currentLayout

                VideoTile {
                    id: tile
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    tileIndex: index
                    channelId: index + 1
                    cameraName: "Camera " + (index + 1)
                    streamType: "SUB"
                    resolutionWidth: root.currentLayout === 1 ? 1920 : 640
                    resolutionHeight: root.currentLayout === 1 ? 1080 : 360
                    isFullscreen: false

                    onFullscreenRequested: function(chId) {
                        root.fullscreenChangeRequested(chId)
                    }
                }
            }
        }
    }
}
