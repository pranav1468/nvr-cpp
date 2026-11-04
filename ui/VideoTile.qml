import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "components"

/**
 * Surveillance Matrix Video Tile
 * Exact pixel replica of web console video stage (web_live_actual.png)
 */
Rectangle {
    id: root

    // Public Properties
    property int tileIndex: 0
    property int channelId: 1
    property string cameraName: "Empty"
    property string streamType: "SUB" // "SUB" or "MAIN"
    property int resolutionWidth: 360
    property int resolutionHeight: 0
    property double fps: 25.0
    property int droppedFrames: 0
    property bool isRecording: false
    property string status: "STANDBY" // "STREAMING", "STANDBY", "CONNECTING", "OFFLINE"
    property bool isOnline: (typeof LiveBridge !== "undefined" && LiveBridge && LiveBridge["online" + root.tileIndex] !== undefined) ? LiveBridge["online" + root.tileIndex] : (status === "STREAMING")
    property bool isFullscreen: false
    property int frameRevision: (typeof LiveBridge !== "undefined" && LiveBridge && LiveBridge["rev" + root.tileIndex] !== undefined) ? LiveBridge["rev" + root.tileIndex] : 0
    property bool isSelected: false
    property bool hwAccelerated: (typeof LiveBridge !== "undefined" && LiveBridge && LiveBridge.hasHardwareGlSink) ? true : false
    property string dynamicResolutionText: {
        if (typeof LiveBridge !== "undefined" && LiveBridge && LiveBridge["res" + root.tileIndex]) {
            return LiveBridge["res" + root.tileIndex];
        }
        var h = videoFrame.sourceSize.height;
        if (h > 0) {
            var pfx = (root.streamType === "MAIN" || root.isFullscreen) ? "MAIN" : "SUB";
            if (h >= 2160) return pfx + " 4K";
            if (h >= 1440) return pfx + " 2K";
            return pfx + " " + h + "p";
        }
        return (root.streamType === "MAIN" || root.isFullscreen) ? "MAIN" : "SUB";
    }

    // Signals
    signal fullscreenRequested(int channelId)
    signal tileSelected(int channelId)

    color: Theme.bgTileStage
    border.color: isSelected ? Theme.accentPrimary : (mouseArea.containsMouse ? Qt.rgba(0.07, 0.4, 0.83, 0.6) : Qt.rgba(0.88, 0.91, 0.94, 0.15))
    border.width: isSelected ? 2 : (mouseArea.containsMouse ? 2 : 1)
    radius: 12
    clip: true

    // 1-second clock timer for video timestamp matching web console
    property string liveClockText: ""
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var now = new Date();
            var y = now.getFullYear();
            var m = ("0" + (now.getMonth() + 1)).slice(-2);
            var d = ("0" + now.getDate()).slice(-2);
            var hh = ("0" + now.getHours()).slice(-2);
            var mm = ("0" + now.getMinutes()).slice(-2);
            var ss = ("0" + now.getSeconds()).slice(-2);
            root.liveClockText = y + "-" + m + "-" + d + " " + hh + ":" + mm + ":" + ss;
        }
    }

    // Video Canvas / Image Surface
    Item {
        id: videoSurface
        anchors.fill: parent

        Loader {
            id: hwSurfaceLoader
            anchors.fill: parent
            active: (typeof LiveBridge !== "undefined" && LiveBridge && LiveBridge.hasHardwareGlSink) ? true : false
            source: active ? "components/HardwareGlVideoSurface.qml" : ""
            visible: active && root.isOnline
        }

        Image {
            id: videoFrame
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            cache: false
            asynchronous: true
            source: (root.isOnline && root.frameRevision > 0 && !hwSurfaceLoader.active) ? "image://live/" + root.tileIndex + "/" + root.frameRevision : ""
            visible: root.isOnline && root.frameRevision > 0 && !hwSurfaceLoader.active
        }

        // Empty / Standby Placeholder State (Matches web console)
        Item {
            anchors.fill: parent
            visible: !root.isOnline || (root.frameRevision === 0 && !hwSurfaceLoader.active)

            ColumnLayout {
                anchors.centerIn: parent
                spacing: root.width < 220 ? 4 : 8

                AppIcon {
                    name: "video"
                    size: root.width < 220 ? 22 : 32
                    color: Qt.rgba(1, 1, 1, 0.35)
                    strokeWidth: 1.5
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "NO CAMERA ON THIS CHANNEL"
                    color: Qt.rgba(1, 1, 1, 0.75)
                    font.pixelSize: root.width < 220 ? 9 : 11
                    font.bold: true
                    font.family: Theme.fontDisplay
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "Assign a camera"
                    color: Qt.rgba(1, 1, 1, 0.5)
                    font.pixelSize: root.width < 220 ? 9 : 11
                    font.underline: true
                    font.family: Theme.fontSans
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }

    // Top Header Overlay Bar (Matches web console)
    Item {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 4

            // Channel Indicator Dot + Title
            Rectangle {
                width: 6
                height: 6
                radius: 3
                color: root.isOnline ? Theme.statusOnline : "#64748b"
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: "CH " + (root.channelId < 10 ? "0" + root.channelId : root.channelId) + ": " + root.cameraName
                color: "#ffffff"
                font.pixelSize: root.width < 220 ? 9 : 10
                font.bold: true
                font.family: Theme.fontMono
                Layout.fillWidth: true
                elide: Text.ElideRight
                Layout.alignment: Qt.AlignVCenter
            }

            // Stream Resolution Badge (e.g. SUB 360p)
            Rectangle {
                color: Qt.rgba(0, 0, 0, 0.75)
                radius: 4
                Layout.preferredHeight: 18
                Layout.preferredWidth: subLabel.implicitWidth + 8
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: subLabel
                    anchors.centerIn: parent
                    text: root.dynamicResolutionText
                    color: Qt.rgba(1, 1, 1, 0.85)
                    font.pixelSize: 9
                    font.bold: true
                    font.family: Theme.fontMono
                }
            }

            // VPU HW Acceleration Badge (Responsive: hidden on narrow tiles < 210px)
            Rectangle {
                visible: root.width >= 210
                color: Qt.rgba(0, 0, 0, 0.75)
                radius: 4
                Layout.preferredHeight: 18
                Layout.preferredWidth: vpuLabel.implicitWidth + 8
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: vpuLabel
                    anchors.centerIn: parent
                    text: "VPU HW"
                    color: Qt.rgba(1, 1, 1, 0.85)
                    font.pixelSize: 9
                    font.bold: true
                    font.family: Theme.fontMono
                }
            }

            // REC Badge (if recording)
            Rectangle {
                visible: root.isRecording
                color: Theme.statusRec
                radius: 4
                Layout.preferredHeight: 18
                Layout.preferredWidth: recLabel.implicitWidth + 10
                Layout.alignment: Qt.AlignVCenter

                RowLayout {
                    anchors.centerIn: parent
                    spacing: 4

                    Rectangle {
                        width: 5
                        height: 5
                        radius: 2.5
                        color: "#ffffff"
                    }

                    Text {
                        id: recLabel
                        text: "REC"
                        color: "#ffffff"
                        font.pixelSize: 9
                        font.bold: true
                        font.family: Theme.fontMono
                    }
                }
            }
        }
    }

    // Bottom Telemetry Overlay Bar (Matches web console)
    Item {
        id: bottomBar
        z: 10
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 5

            // Timestamp (e.g. 2026-11-01 20:35:46)
            Text {
                text: root.liveClockText
                color: Qt.rgba(1, 1, 1, 0.75)
                font.pixelSize: root.width < 220 ? 9 : 10
                font.family: Theme.fontMono
                Layout.alignment: Qt.AlignVCenter
            }

            Item { Layout.fillWidth: true }

            // Standby / FPS Badge (visible on wider tiles)
            Rectangle {
                visible: root.width >= 250
                color: Qt.rgba(0, 0, 0, 0.75)
                radius: 4
                Layout.preferredHeight: 20
                Layout.preferredWidth: statusLabel.implicitWidth + 10
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: statusLabel
                    anchors.centerIn: parent
                    text: root.isOnline ? (root.fps.toFixed(1) + " FPS") : "Standby"
                    color: Qt.rgba(1, 1, 1, 0.85)
                    font.pixelSize: 9
                    font.bold: true
                    font.family: Theme.fontMono
                }
            }

            // Max-Min (Fullscreen / Restore) Action Icon Button
            Rectangle {
                id: maxMinBtn
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                radius: 6
                color: maxMinBtnMouse.containsMouse ? Theme.accentPrimary : Qt.rgba(0, 0, 0, 0.65)
                border.color: maxMinBtnMouse.containsMouse ? Theme.accentPrimary : Qt.rgba(1, 1, 1, 0.25)
                border.width: 1
                opacity: mouseArea.containsMouse || maxMinBtnMouse.containsMouse ? 1.0 : 0.85
                Layout.alignment: Qt.AlignVCenter

                Behavior on color { ColorAnimation { duration: 120 } }
                Behavior on opacity { NumberAnimation { duration: 120 } }

                AppIcon {
                    name: root.isFullscreen ? "minimize" : "expand"
                    size: 13
                    color: "#ffffff"
                    strokeWidth: 1.75
                    anchors.centerIn: parent
                }

                MouseArea {
                    id: maxMinBtnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.fullscreenRequested(root.channelId);
                    }
                }
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
