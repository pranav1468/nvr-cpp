import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: root

    // Public Properties
    property int tileIndex: 0
    property int channelId: 1
    property string cameraName: "Camera " + channelId
    property string streamType: "SUB" // "SUB" or "MAIN"
    property int resolutionWidth: 640
    property int resolutionHeight: 360
    property double fps: 25.0
    property int droppedFrames: 0
    property bool isRecording: false
    property bool isOnline: true
    property bool isFullscreen: false
    property int frameRevision: 0

    // Signals
    signal fullscreenRequested(int channelId)
    signal tileSelected(int channelId)

    color: "#0a0f1d"
    border.color: mouseArea.containsMouse ? "#38bdf8" : "#1e293b"
    border.width: mouseArea.containsMouse ? 2 : 1
    radius: 6
    clip: true

    // Video Canvas / Image Surface
    Item {
        id: videoSurface
        anchors.fill: parent
        anchors.margins: 1

        Image {
            id: videoFrame
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            cache: false
            asynchronous: true
            source: root.isOnline ? "image://live/" + root.tileIndex + "/" + root.frameRevision : ""
            visible: root.isOnline
        }

        // Offline / Connecting Placeholder
        Rectangle {
            anchors.fill: parent
            color: "#070b14"
            visible: !root.isOnline

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8

                Text {
                    text: "SIGNAL LOST"
                    color: "#ef4444"
                    font.pixelSize: 13
                    font.bold: true
                    font.family: "Monospace"
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "Reconnecting Channel " + root.channelId + "..."
                    color: "#64748b"
                    font.pixelSize: 11
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }

    // Top Header Overlay Bar (Auto-fades or stays subtle)
    Rectangle {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32
        color: Qt.rgba(0.04, 0.07, 0.13, 0.82)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            // Channel & Name Badge
            Rectangle {
                color: "#1e293b"
                radius: 4
                Layout.preferredHeight: 20
                Layout.preferredWidth: channelLabel.implicitWidth + 12

                Text {
                    id: channelLabel
                    anchors.centerIn: parent
                    text: "CAM " + (root.channelId < 10 ? "0" + root.channelId : root.channelId)
                    color: "#f8fafc"
                    font.pixelSize: 10
                    font.bold: true
                    font.family: "Monospace"
                }
            }

            Text {
                text: root.cameraName
                color: "#94a3b8"
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            // Stream Type (SUB vs MAIN) Badge
            Rectangle {
                color: root.streamType === "MAIN" ? "#065f46" : "#1e293b"
                radius: 4
                Layout.preferredHeight: 18
                Layout.preferredWidth: streamLabel.implicitWidth + 8

                Text {
                    id: streamLabel
                    anchors.centerIn: parent
                    text: root.streamType
                    color: root.streamType === "MAIN" ? "#34d399" : "#94a3b8"
                    font.pixelSize: 9
                    font.bold: true
                    font.family: "Monospace"
                }
            }

            // REC Badge
            Rectangle {
                visible: root.isRecording
                color: "#7f1d1d"
                radius: 4
                Layout.preferredHeight: 18
                Layout.preferredWidth: recLabel.implicitWidth + 8

                RowLayout {
                    anchors.centerIn: parent
                    spacing: 4

                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        color: "#ef4444"
                    }

                    Text {
                        id: recLabel
                        text: "REC"
                        color: "#fca5a5"
                        font.pixelSize: 9
                        font.bold: true
                        font.family: "Monospace"
                    }
                }
            }

            // Fullscreen Button
            Rectangle {
                id: expandBtn
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 4
                color: expandMouse.containsMouse ? "#334155" : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: root.isFullscreen ? "⤓" : "⤢"
                    color: expandMouse.containsMouse ? "#38bdf8" : "#94a3b8"
                    font.pixelSize: 14
                }

                MouseArea {
                    id: expandMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.fullscreenRequested(root.channelId)
                }
            }
        }
    }

    // Bottom Telemetry Overlay Bar
    Rectangle {
        id: bottomBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 22
        color: Qt.rgba(0.04, 0.07, 0.13, 0.75)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 12

            Text {
                text: root.resolutionWidth + "x" + root.resolutionHeight
                color: "#64748b"
                font.pixelSize: 10
                font.family: "Monospace"
            }

            Text {
                text: root.fps.toFixed(1) + " FPS"
                color: "#38bdf8"
                font.pixelSize: 10
                font.family: "Monospace"
            }

            Item { Layout.fillWidth: true }

            Text {
                visible: root.droppedFrames > 0
                text: "Drops: " + root.droppedFrames
                color: "#eab308"
                font.pixelSize: 10
                font.family: "Monospace"
            }
        }
    }

    // Mouse Interaction for Grid Selection & Fullscreen Double-Click
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        onClicked: {
            root.tileSelected(root.channelId)
        }

        onDoubleClicked: {
            root.fullscreenRequested(root.channelId)
        }
    }
}
