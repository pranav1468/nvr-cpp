import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root

    property bool isOpen: false
    property var currentCamera: null
    property string activeTab: "encoder" // "encoder", "maintenance"
    property string statusMessage: ""
    property bool isBusy: false

    signal closed()

    anchors.fill: parent
    color: "#000000bb"
    visible: isOpen
    z: 9999

    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    function open(cam) {
        root.currentCamera = cam;
        root.activeTab = "encoder";
        root.statusMessage = "Loading camera encoder settings...";
        root.isOpen = true;

        if (cam && cam.id) {
            root.isBusy = true;
            ApiClient.getEncoder({ channel_id: cam.id }, function(res) {
                root.isBusy = false;
                root.statusMessage = "";
                var enc = (res && res.data && res.data.config) ? res.data.config : ((res && res.config) ? res.config : null);
                if (enc) {
                    if (enc.encoding === "H265") codecCombo.currentIndex = 1;
                    else codecCombo.currentIndex = 0;

                    var curW = enc.width || 1920;
                    var curH = enc.height || 1080;
                    var resStr = curW + "x" + curH;
                    if (resStr.indexOf("3840") !== -1) resolutionCombo.currentIndex = 1;
                    else if (resStr.indexOf("1280") !== -1) resolutionCombo.currentIndex = 2;
                    else if (resStr.indexOf("640") !== -1) resolutionCombo.currentIndex = 3;
                    else resolutionCombo.currentIndex = 0;

                    var curFps = enc.fps || 25;
                    if (curFps === 15) fpsCombo.currentIndex = 1;
                    else if (curFps === 20) fpsCombo.currentIndex = 2;
                    else if (curFps === 30) fpsCombo.currentIndex = 3;
                    else fpsCombo.currentIndex = 0;

                    if (enc.bitrate_kbps) bitrateInput.text = String(enc.bitrate_kbps);
                    if (enc.gov_length) gopInput.text = String(enc.gov_length);
                }
            }, function(err) {
                root.isBusy = false;
                root.statusMessage = "Using active camera defaults";
            });
        }
    }

    function close() {
        root.isOpen = false;
        root.closed();
    }

    function saveEncoder() {
        if (!root.currentCamera) return;
        var resParts = resolutionCombo.currentText.split(" ")[0].split("x");
        var mediaAddr = root.currentCamera.onvif_xaddr || root.currentCamera.xaddr || (root.currentCamera.ip_address ? ("http://" + root.currentCamera.ip_address + "/onvif/media_service") : "");
        var payload = {
            channel_id: root.currentCamera.id || 0,
            media_xaddr: mediaAddr,
            encoder_token: root.currentCamera.video_encoder_token || "EncoderToken_Main",
            username: root.currentCamera.username || "",
            password: root.currentCamera.password || "",
            encoding: codecCombo.currentText === "H.265 / HEVC" ? "H265" : "H264",
            width: parseInt(resParts[0], 10) || 1920,
            height: parseInt(resParts[1], 10) || 1080,
            fps: parseInt(fpsCombo.currentText.split(" ")[0], 10) || 25,
            bitrate_kbps: parseInt(bitrateInput.text, 10) || 4000,
            gov_length: parseInt(gopInput.text, 10) || 50,
            h264_profile: profileCombo.currentText
        };

        root.isBusy = true;
        root.statusMessage = "Dispatching ONVIF SetVideoEncoderConfiguration command...";

        ApiClient.setEncoder(payload, function() {
            root.isBusy = false;
            root.statusMessage = "Camera hardware encoder successfully reconfigured!";
        }, function(err) {
            root.isBusy = false;
            root.statusMessage = "Failed to update camera hardware encoder: " + err;
        });
    }


    function syncTime() {
        if (!root.currentCamera) return;
        var now = new Date();
        var devAddr = root.currentCamera.xaddr || (root.currentCamera.ip_address ? ("http://" + root.currentCamera.ip_address + "/onvif/device_service") : "");
        var payload = {
            device_xaddr: devAddr,
            username: root.currentCamera.username || "",
            password: root.currentCamera.password || "",
            year: now.getUTCFullYear(),
            month: now.getUTCMonth() + 1,
            day: now.getUTCDate(),
            hour: now.getUTCHours(),
            minute: now.getUTCMinutes(),
            second: now.getUTCSeconds()
        };

        ApiClient.setDateTime(payload, function() {
            root.statusMessage = "Camera hardware clock synchronized with NVR system time.";
        }, function(err) {
            root.statusMessage = "Time sync failed: " + err;
        });
    }

    function rebootCamera() {
        if (!root.currentCamera) return;
        var devAddr = root.currentCamera.xaddr || (root.currentCamera.ip_address ? ("http://" + root.currentCamera.ip_address + "/onvif/device_service") : "");
        var payload = {
            device_xaddr: devAddr,
            username: root.currentCamera.username || "",
            password: root.currentCamera.password || ""
        };
        ApiClient.rebootCamera(payload, function() {
            root.statusMessage = "Reboot command dispatched. Camera hardware is restarting...";
        }, function(err) {
            root.statusMessage = "Reboot command failed: " + err;
        });
    }

    // Modal Card
    Rectangle {
        anchors.centerIn: parent
        width: 640
        height: 520
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
                    text: "ONVIF Hardware Camera Configuration: " + (root.currentCamera ? (root.currentCamera.name + " (CH " + root.currentCamera.id + ")") : "")
                    color: Theme.textPrimary
                    font.pixelSize: 14
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

            // Tabs Bar
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    model: [
                        { key: "encoder", label: "Video Encoder (Codec & Stream)" },
                        { key: "maintenance", label: "Maintenance & Reboot" }
                    ]
                    Rectangle {
                        Layout.preferredHeight: 30
                        Layout.preferredWidth: 180
                        color: root.activeTab === modelData.key ? "#1d4ed833" : Theme.bgInput
                        border.color: root.activeTab === modelData.key ? Theme.accentBlue : Theme.borderSubtle
                        radius: Theme.radiusSm

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: root.activeTab === modelData.key ? Theme.accentBlue : Theme.textSecondary
                            font.pixelSize: 11
                            font.bold: root.activeTab === modelData.key
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activeTab = modelData.key
                        }
                    }
                }
            }

            // Tab 1: Video Encoder
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.activeTab === "encoder"
                spacing: 10

                Text {
                    text: "Reconfigure physical camera's on-board DSP encoder over ONVIF Media Service:"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Compression Codec"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: codecCombo
                            Layout.fillWidth: true; height: 32
                            model: ["H.264 (Hardware Standard)", "H.265 / HEVC"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: codecCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Resolution"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: resolutionCombo
                            Layout.fillWidth: true; height: 32
                            model: ["1920x1080 (1080p FHD)", "3840x2160 (4K UHD)", "1280x720 (720p HD)", "640x360 (360p Sub)"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: resolutionCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Frame Rate (FPS)"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: fpsCombo
                            Layout.fillWidth: true; height: 32
                            model: ["25 FPS (PAL Standard)", "15 FPS", "20 FPS", "30 FPS (NTSC Standard)"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: fpsCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "H.264 Profile"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: profileCombo
                            Layout.fillWidth: true; height: 32
                            model: ["Main", "High", "Baseline"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: profileCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Bitrate Target (kbps)"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: bitrateInput
                                anchors.fill: parent; anchors.margins: 6
                                text: "4000"
                                color: Theme.textPrimary; font.family: Theme.fontMono; font.pixelSize: 12
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "Keyframe Interval (GOP Length)"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: gopInput
                                anchors.fill: parent; anchors.margins: 6
                                text: "50"
                                color: Theme.textPrimary; font.family: Theme.fontMono; font.pixelSize: 12
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.alignment: Qt.AlignRight
                    Layout.preferredWidth: 240
                    Layout.preferredHeight: 34
                    color: Theme.accentBlue
                    radius: Theme.radiusSm
                    Text {
                        anchors.centerIn: parent
                        text: "⚡ Apply Encoder Settings to Camera"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.saveEncoder()
                    }
                }
            }


            // Tab 3: Maintenance
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.activeTab === "maintenance"
                spacing: 16

                Rectangle {
                    Layout.fillWidth: true
                    height: 90
                    color: Theme.bgElevated
                    border.color: Theme.borderSubtle
                    radius: Theme.radiusSm

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Text { text: "Time Synchronization (NTP / RTC)"; color: Theme.textPrimary; font.pixelSize: 12; font.bold: true }
                        Text { text: "Synchronize camera hardware clock to NVR time to maintain exact frame timestamp alignment."; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.preferredWidth: 200
                            Layout.preferredHeight: 28
                            color: Theme.bgInput
                            border.color: Theme.borderSubtle
                            radius: Theme.radiusSm
                            Text { anchors.centerIn: parent; text: "🕒 Sync Camera Clock to NVR"; color: Theme.textPrimary; font.pixelSize: 11 }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.syncTime() }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 90
                    color: Theme.bgElevated
                    border.color: Theme.borderSubtle
                    radius: Theme.radiusSm

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Text { text: "Remote Camera Hardware Reboot"; color: Theme.statusDanger; font.pixelSize: 12; font.bold: true }
                        Text { text: "Instruct the physical camera microcontroller to perform a cold hardware restart over ONVIF."; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.preferredWidth: 220
                            Layout.preferredHeight: 28
                            color: "#ef444433"
                            border.color: Theme.statusDanger
                            radius: Theme.radiusSm
                            Text { anchors.centerIn: parent; text: "⟳ Send ONVIF Reboot Command"; color: Theme.statusDanger; font.pixelSize: 11; font.bold: true }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.rebootCamera() }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }

            // Status notification text
            Text {
                Layout.fillWidth: true
                text: root.statusMessage
                color: Theme.statusWarning
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            // Close button
            Rectangle {
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: 90
                Layout.preferredHeight: 32
                color: Theme.bgInput
                border.color: Theme.borderSubtle
                radius: Theme.radiusSm
                Text {
                    anchors.centerIn: parent
                    text: "Close"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }
    }
}
