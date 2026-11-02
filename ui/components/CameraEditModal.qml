import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root

    property bool isOpen: false
    property bool isEditMode: false
    property var editingCamera: null
    property string statusMessage: ""
    property bool isBusy: false

    signal saved()
    signal closed()

    anchors.fill: parent
    color: "#000000bb"
    visible: isOpen
    z: 9999

    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    function openNew() {
        root.isEditMode = false;
        root.editingCamera = null;
        camNameField.text = "";
        mainUrlField.text = "";
        subUrlField.text = "";
        userField.text = "";
        passField.text = "";
        fpsCombo.currentIndex = 2; // 25 FPS
        root.statusMessage = "";
        root.isOpen = true;
    }

    function openEdit(cam) {
        root.isEditMode = true;
        root.editingCamera = cam;
        camNameField.text = (cam && cam.name) ? cam.name : "";
        mainUrlField.text = (cam && (cam.rtsp_url_main || cam.rtsp_url)) ? (cam.rtsp_url_main || cam.rtsp_url) : "";
        subUrlField.text = (cam && (cam.rtsp_url_sub || cam.sub_rtsp_url)) ? (cam.rtsp_url_sub || cam.sub_rtsp_url) : "";
        userField.text = (cam && cam.username) ? cam.username : "admin";
        passField.text = "";
        fpsCombo.currentIndex = (cam && cam.fps === 30) ? 3 : ((cam && cam.fps === 20) ? 1 : ((cam && cam.fps === 15) ? 0 : 2));
        root.statusMessage = "";
        root.isOpen = true;
    }

    function close() {
        root.isOpen = false;
        root.closed();
    }

    function saveCamera() {
        var payload = {
            name: camNameField.text,
            rtsp_url: mainUrlField.text,
            sub_rtsp_url: subUrlField.text,
            username: userField.text,
            fps: parseInt(fpsCombo.currentText, 10) || 25
        };
        if (passField.text.length > 0) {
            payload.password = passField.text;
        }

        root.isBusy = true;
        root.statusMessage = "Saving camera configuration...";

        if (root.isEditMode && root.editingCamera) {
            ApiClient.updateCamera(root.editingCamera.id, payload, function() {
                root.isBusy = false;
                root.saved();
                root.close();
            }, function(err) {
                root.isBusy = false;
                root.statusMessage = "Save failed: " + err;
            });
        } else {
            ApiClient.addCamera(payload, function() {
                root.isBusy = false;
                root.saved();
                root.close();
            }, function(err) {
                root.isBusy = false;
                root.statusMessage = "Add failed: " + err;
            });
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 540
        height: 480
        color: Theme.bgSurface
        border.color: Theme.borderSubtle
        radius: Theme.radiusLg
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            // Header
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: root.isEditMode ? ("Configure Channel " + (root.editingCamera ? root.editingCamera.id : "")) : "Add New Camera Stream"
                    color: Theme.textPrimary
                    font.pixelSize: 15
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 26
                    height: 26
                    color: Theme.bgInput
                    radius: 4
                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }

            // Form inputs
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10

                Text { text: "Camera Name"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: camNameField
                        anchors.fill: parent; anchors.margins: 6
                        color: Theme.textPrimary; font.pixelSize: 12
                    }
                }

                Text { text: "RTSP Main Stream URL (1080p Recording - Zero Transcode)"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: mainUrlField
                        anchors.fill: parent; anchors.margins: 6
                        color: Theme.textPrimary; font.family: Theme.fontMono; font.pixelSize: 11
                    }
                }

                Text { text: "RTSP Sub Stream URL (Live Grid & AI Analytics)"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: subUrlField
                        anchors.fill: parent; anchors.margins: 6
                        color: Theme.textPrimary; font.family: Theme.fontMono; font.pixelSize: 11
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "ONVIF/RTSP Username"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: userField
                                anchors.fill: parent; anchors.margins: 6
                                text: "admin"
                                color: Theme.textPrimary; font.pixelSize: 12
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "ONVIF/RTSP Password"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: passField
                                anchors.fill: parent; anchors.margins: 6
                                echoMode: TextInput.Password
                                text: ""
                                color: Theme.textPrimary; font.pixelSize: 12
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Target Frame Rate"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: fpsCombo
                            Layout.fillWidth: true; height: 32
                            model: ["15 FPS", "20 FPS", "25 FPS", "30 FPS"]
                            currentIndex: 2
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: fpsCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Bitrate Codec"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: codecCombo
                            Layout.fillWidth: true; height: 32
                            model: ["H.264 (Pass-Through)", "H.265 / HEVC"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: codecCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: root.statusMessage
                color: Theme.statusWarning
                font.pixelSize: 11
            }

            // Buttons
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 34
                    color: Theme.bgInput
                    border.color: Theme.borderSubtle
                    radius: Theme.radiusSm
                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 130
                    Layout.preferredHeight: 34
                    color: Theme.accentBlue
                    radius: Theme.radiusSm

                    Text {
                        anchors.centerIn: parent
                        text: root.isEditMode ? "Save Changes" : "Add Camera"
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.saveCamera()
                    }
                }
            }
        }
    }
}
