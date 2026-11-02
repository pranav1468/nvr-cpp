import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root

    property var cameras: []
    property string statusMessage: ""

    Component.onCompleted: {
        loadCameras();
    }

    function loadCameras() {
        ApiClient.getCameras(function(list) {
            root.cameras = list || [];
        }, function(err) {
            root.statusMessage = "Failed to load cameras: " + err;
        });
    }

    function restartCamera(id) {
        ApiClient.restartCamera(id, function() {
            root.statusMessage = "Restarted ingress session for Channel " + id;
        }, function(err) {
            root.statusMessage = "Restart failed: " + err;
        });
    }

    function deleteCamera(id) {
        ApiClient.deleteCamera(id, function() {
            root.statusMessage = "Camera " + id + " deleted";
            loadCameras();
        }, function(err) {
            root.statusMessage = "Delete failed: " + err;
        });
    }

    function toggleRecording(cam) {
        if (!cam) return;
        var currentlyRecording = (cam.is_recording !== false);
        if (currentlyRecording) {
            root.statusMessage = "Stopping recording for Channel " + cam.id + "...";
            ApiClient.stopRecording(cam.id, function() {
                cam.is_recording = false;
                root.statusMessage = "Recording stopped for Channel " + cam.id;
                loadCameras();
            }, function(err) {
                root.statusMessage = "Stop recording failed: " + err;
            });
        } else {
            root.statusMessage = "Starting recording for Channel " + cam.id + "...";
            ApiClient.startRecording(cam.id, "continuous", function() {
                cam.is_recording = true;
                root.statusMessage = "Recording started for Channel " + cam.id;
                loadCameras();
            }, function(err) {
                root.statusMessage = "Start recording failed: " + err;
            });
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        // 1. Page Header (Exact Web Parity)
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true

                Text {
                    text: "Camera ingress management"
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.bold: true
                    font.family: Theme.fontDisplay
                }
                Text {
                    text: "Centralized ingress sessions · sub & main stream profiles"
                    color: Theme.textMuted
                    font.pixelSize: 13
                    font.family: Theme.fontSans
                }
            }

            // Action Buttons
            RowLayout {
                spacing: 8

                // Refresh Button
                Rectangle {
                    Layout.preferredHeight: 32
                    Layout.preferredWidth: refreshBtnRow.implicitWidth + 24
                    color: Theme.bgSecondary
                    border.color: Theme.borderSubtle
                    radius: 6

                    RowLayout {
                        id: refreshBtnRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "refresh"
                            size: 13
                            color: Theme.textSecondary
                            strokeWidth: 2
                        }

                        Text {
                            text: "Refresh"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.loadCameras()
                    }
                }

                // Add Manually Button
                Rectangle {
                    Layout.preferredHeight: 32
                    Layout.preferredWidth: addBtnRow.implicitWidth + 24
                    color: Theme.bgSecondary
                    border.color: Theme.borderSubtle
                    radius: 6

                    RowLayout {
                        id: addBtnRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "plus"
                            size: 13
                            color: Theme.textSecondary
                            strokeWidth: 2
                        }

                        Text {
                            text: "Add manually"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: editModal.openNew()
                    }
                }

                // Auto-discover ONVIF Button
                Rectangle {
                    Layout.preferredHeight: 32
                    Layout.preferredWidth: onvifBtnRow.implicitWidth + 24
                    color: Theme.accentPrimary
                    radius: 6

                    RowLayout {
                        id: onvifBtnRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "wifi"
                            size: 14
                            color: "#ffffff"
                            strokeWidth: 2
                        }

                        Text {
                            text: "Auto-discover ONVIF"
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: onvifWizard.open()
                    }
                }
            }
        }

        // Status banner if any
        Text {
            Layout.fillWidth: true
            visible: root.statusMessage.length > 0
            text: root.statusMessage
            color: Theme.accentBlue
            font.pixelSize: 11
        }

        // 2. Camera Grid / Empty State Area
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Left-aligned Empty State Card (Exact Web Parity with camera grid)
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                width: 350
                height: 380
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 16
                visible: root.cameras.length === 0

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 14
                    width: parent.width - 48

                    AppIcon {
                        name: "video"
                        size: 40
                        color: Theme.textDim
                        strokeWidth: 1.75
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Text {
                        text: "No cameras added yet"
                        color: Theme.textPrimary
                        font.pixelSize: 18
                        font.bold: true
                        font.family: Theme.fontDisplay
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Text {
                        text: "Discover ONVIF compliant cameras on the local subnet or configure an RTSP stream manually."
                        color: Theme.textMuted
                        font.pixelSize: 13
                        font.family: Theme.fontSans
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 4
                        width: runOnvifRow.implicitWidth + 28
                        height: 36
                        color: Theme.accentPrimary
                        radius: 6

                        RowLayout {
                            id: runOnvifRow
                            anchors.centerIn: parent
                            spacing: 8

                            AppIcon {
                                name: "lightning"
                                size: 13
                                color: "#ffffff"
                                strokeWidth: 2
                            }

                            Text {
                                text: "Run ONVIF network discovery"
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: onvifWizard.open()
                        }
                    }
                }
            }

            // Grid of Cameras
            ScrollView {
                anchors.fill: parent
                visible: root.cameras.length > 0
                clip: true

                GridView {
                    id: cameraGridView
                    width: parent.width
                    cellWidth: width > 1100 ? (width / 4) : (width > 800 ? (width / 3) : (width / 2))
                    cellHeight: 345
                    model: root.cameras

                    delegate: Item {
                        width: cameraGridView.cellWidth
                        height: cameraGridView.cellHeight

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 8
                            color: Theme.bgSurface
                            border.color: Theme.borderSubtle
                            radius: Theme.radiusLg
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                // Camera Stage Thumbnail (16:9)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 130
                                    color: Theme.bgTileStage

                                    Image {
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectFit
                                        source: ApiClient.getSnapshotUrl(modelData.id)
                                    }

                                    // CH Badge
                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.margins: 6
                                        width: 50
                                        height: 20
                                        color: "#000000aa"
                                        border.color: Theme.accentBlue
                                        radius: 3
                                        Text {
                                            anchors.centerIn: parent
                                            text: "CH 0" + modelData.id
                                            color: Theme.accentBlue
                                            font.family: Theme.fontMono
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }

                                    // Online status badge
                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        width: 60
                                        height: 20
                                        color: (modelData.status === "OFFLINE") ? "#334155" : "#10b98133"
                                        border.color: (modelData.status === "OFFLINE") ? "#475569" : Theme.statusOnline
                                        radius: 3
                                        Text {
                                            anchors.centerIn: parent
                                            text: (modelData.status === "OFFLINE") ? "OFFLINE" : "ONLINE"
                                            color: (modelData.status === "OFFLINE") ? "#94a3b8" : Theme.statusOnline
                                            font.pixelSize: 9
                                            font.bold: true
                                        }
                                    }
                                }

                                // Card Body Details
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: Theme.bgSurface

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 6

                                        // Name and IP
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: modelData.name || ("Camera " + modelData.id)
                                                color: Theme.textPrimary
                                                font.pixelSize: 13
                                                font.bold: true
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: modelData.ip_address || "192.168.1.x"
                                                color: Theme.textMuted
                                                font.family: Theme.fontMono
                                                font.pixelSize: 10
                                            }
                                        }

                                        // Details Grid
                                        GridLayout {
                                            columns: 2
                                            Layout.fillWidth: true
                                            rowSpacing: 2
                                            columnSpacing: 8

                                            Text { text: "Resolution:"; color: Theme.textMuted; font.pixelSize: 10 }
                                            Text { text: modelData.resolution || "1080p"; color: Theme.textPrimary; font.pixelSize: 10; font.family: Theme.fontMono }

                                            Text { text: "Frame Rate:"; color: Theme.textMuted; font.pixelSize: 10 }
                                            Text { text: (modelData.fps || 25) + " FPS"; color: Theme.textPrimary; font.pixelSize: 10; font.family: Theme.fontMono }

                                            Text { text: "Bitrate:"; color: Theme.textMuted; font.pixelSize: 10 }
                                            Text { text: modelData.bitrate_kbps ? (modelData.bitrate_kbps + "k") : "Auto"; color: Theme.textPrimary; font.pixelSize: 10; font.family: Theme.fontMono }
                                        }

                                        // RTSP Url
                                        Text {
                                            Layout.fillWidth: true
                                            text: "RTSP: " + (modelData.rtsp_url_sub || modelData.rtsp_url || "Not configured")
                                            color: Theme.textMuted
                                            font.family: Theme.fontMono
                                            font.pixelSize: 9
                                            elide: Text.ElideMiddle
                                        }

                                        Item { Layout.fillHeight: true }

                                        // Actions - Row 1 (Hardware & Recording Configuration)
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6

                                            // Record / Stop Manual Toggle Button
                                            Rectangle {
                                                Layout.fillWidth: true
                                                height: 26
                                                color: (modelData.is_recording !== false) ? "#ef444422" : "#10b98122"
                                                border.color: (modelData.is_recording !== false) ? "#ef4444" : "#10b981"
                                                radius: 3
                                                RowLayout {
                                                    anchors.centerIn: parent
                                                    spacing: 4
                                                    Rectangle {
                                                        width: 6; height: 6; radius: 3
                                                        color: (modelData.is_recording !== false) ? "#ef4444" : "#10b981"
                                                    }
                                                    Text {
                                                        text: (modelData.is_recording !== false) ? "Stop Rec" : "Start Rec"
                                                        color: (modelData.is_recording !== false) ? "#ef4444" : "#10b981"
                                                        font.pixelSize: 10
                                                        font.bold: true
                                                        font.family: Theme.fontSans
                                                    }
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: root.toggleRecording(modelData)
                                                }
                                            }

                                            Rectangle {
                                                Layout.fillWidth: true
                                                height: 26
                                                color: Theme.bgInput
                                                border.color: Theme.borderSubtle
                                                radius: 3
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "⚙ HW Config"
                                                    color: Theme.accentBlue
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: hwModal.open(modelData)
                                                }
                                            }
                                        }

                                        // Actions - Row 2 (Operations)
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6

                                            Rectangle {
                                                Layout.fillWidth: true
                                                height: 26
                                                color: Theme.bgInput
                                                border.color: Theme.borderSubtle
                                                radius: 3
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "⟳ Restart"
                                                    color: Theme.textPrimary
                                                    font.pixelSize: 10
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: root.restartCamera(modelData.id)
                                                }
                                            }

                                            Rectangle {
                                                width: 50
                                                height: 26
                                                color: Theme.bgInput
                                                border.color: Theme.borderSubtle
                                                radius: 3
                                                RowLayout {
                                                    anchors.centerIn: parent
                                                    spacing: 4
                                                    Image { source: "../assets/images/edit.png"; width: 10; height: 10 }
                                                    Text { text: "Edit"; color: Theme.textPrimary; font.pixelSize: 10 }
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: editModal.openEdit(modelData)
                                                }
                                            }

                                            Rectangle {
                                                width: 32
                                                height: 26
                                                color: "#ef444422"
                                                border.color: "#ef4444"
                                                radius: 3
                                                Image {
                                                    anchors.centerIn: parent
                                                    source: "../assets/images/delete.png"
                                                    width: 10
                                                    height: 10
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: root.deleteCamera(modelData.id)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Modal Overlays
    OnvifDiscoveryModal {
        id: onvifWizard
        onCameraAdded: root.loadCameras()
    }

    CameraEditModal {
        id: editModal
        onSaved: root.loadCameras()
    }

    CameraHardwareModal {
        id: hwModal
    }
}
