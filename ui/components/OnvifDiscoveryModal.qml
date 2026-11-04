import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root

    property bool isOpen: false
    property int currentStep: 1 // 1: Discover, 2: Credentials & Profiles
    property var discoveredDevices: []
    property var selectedDevice: null
    property string statusMessage: ""
    property bool isBusy: false

    signal cameraAdded()
    signal closed()

    anchors.fill: parent
    color: "#000000bb"
    visible: isOpen
    z: 9999

    MouseArea {
        anchors.fill: parent
        onClicked: {} // Block clicks outside
    }

    function open() {
        root.isOpen = true;
        root.currentStep = 1;
        root.discoveredDevices = [];
        root.selectedDevice = null;
        root.statusMessage = "";
        runDiscovery();
    }

    function close() {
        root.isOpen = false;
        root.closed();
    }

    function runDiscovery() {
        root.isBusy = true;
        root.statusMessage = "Broadcasting WS-Discovery probes on local network...";
        ApiClient.discoverOnvif(3, function(res) {
            root.isBusy = false;
            var devs = (res && res.devices) ? res.devices : ((res && res.data) ? res.data : []);
            root.discoveredDevices = devs;
            if (devs.length === 0) {
                root.statusMessage = "No ONVIF cameras responded. Ensure cameras are powered and on the same subnet.";
            } else {
                root.statusMessage = "Found " + devs.length + " ONVIF compliant camera(s).";
            }
        }, function(err) {
            root.isBusy = false;
            root.statusMessage = "Discovery error: " + err;
        });
    }

    function probeProfiles() {
        if (!root.selectedDevice) return;
        root.isBusy = true;
        root.statusMessage = "Connecting to ONVIF service and reading media profiles...";

        var payload = {
            endpoint: root.selectedDevice.xaddr || root.selectedDevice.endpoint || ("http://" + root.selectedDevice.ip + "/onvif/device_service"),
            username: onvifUsernameInput.text,
            password: onvifPasswordInput.text
        };

        ApiClient.probeOnvif(payload, function(res) {
            root.isBusy = false;
            var profileData = (res && res.data) ? res.data : res;
            mainUrlInput.text = (profileData && profileData.main_rtsp_url) ? profileData.main_rtsp_url : "";
            subUrlInput.text = (profileData && profileData.sub_rtsp_url) ? profileData.sub_rtsp_url : "";
            camNameInput.text = (root.selectedDevice.model ? (root.selectedDevice.manufacturer + " " + root.selectedDevice.model) : "Discovered Camera");
            root.currentStep = 2;
            root.statusMessage = "Media profiles resolved successfully.";
        }, function(err) {
            root.isBusy = false;
            root.statusMessage = "Probe failed: " + err + ". Check username and password.";
        });
    }

    function commitCamera() {
        var camConfig = {
            name: camNameInput.text,
            rtsp_url: mainUrlInput.text,
            sub_rtsp_url: subUrlInput.text,
            username: onvifUsernameInput.text,
            password: onvifPasswordInput.text,
            fps: parseInt(fpsCombo.currentText, 10) || 25
        };

        root.isBusy = true;
        root.statusMessage = "Saving camera to ingress engine...";
        ApiClient.addCamera(camConfig, function() {
            root.isBusy = false;
            root.cameraAdded();
            root.close();
        }, function(err) {
            root.isBusy = false;
            root.statusMessage = "Failed to add camera: " + err;
        });
    }

    // Modal Card
    Rectangle {
        id: modalCard
        anchors.centerIn: parent
        width: 620
        height: root.currentStep === 1 ? 480 : 520
        color: Theme.bgSurface
        border.color: Theme.borderSubtle
        radius: Theme.radiusLg
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 16

            // Header
            RowLayout {
                Layout.fillWidth: true
                Image {
                    source: "../assets/images/camera.png"
                    width: 20
                    height: 20
                }
                Text {
                    text: root.currentStep === 1 ? "ONVIF Auto-Discovery Wizard (Step 1/2)" : "Configure Stream Profiles (Step 2/2)"
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

            // Step 1: Discovered Devices
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.currentStep === 1
                spacing: 12

                Text {
                    text: "Select a camera discovered on the local network to fetch its stream profiles:"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#080c18"
                    border.color: Theme.borderSubtle
                    radius: Theme.radiusMd
                    clip: true

                    ListView {
                        id: devList
                        anchors.fill: parent
                        anchors.margins: 6
                        model: root.discoveredDevices
                        spacing: 4

                        delegate: Rectangle {
                            width: devList.width
                            height: 52
                            color: root.selectedDevice === modelData ? "#1e293b" : "#0f172a"
                            border.color: root.selectedDevice === modelData ? Theme.accentBlue : Theme.borderSubtle
                            radius: Theme.radiusSm

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 12

                                Text {
                                    text: "📹"
                                    font.pixelSize: 16
                                }
                                ColumnLayout {
                                    spacing: 2
                                    Text {
                                        text: (modelData.manufacturer || "Generic") + " " + (modelData.model || "ONVIF Camera")
                                        color: Theme.textPrimary
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    Text {
                                        text: "IP: " + (modelData.ip || "Unassigned") + " • MAC: " + (modelData.mac || "N/A")
                                        color: Theme.textMuted
                                        font.family: Theme.fontMono
                                        font.pixelSize: 11
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    width: 16
                                    height: 16
                                    radius: 8
                                    color: root.selectedDevice === modelData ? Theme.accentBlue : "transparent"
                                    border.color: root.selectedDevice === modelData ? Theme.accentBlue : Theme.borderSubtle
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectedDevice = modelData
                            }
                        }
                    }

                    // Empty or busy state
                    Text {
                        anchors.centerIn: parent
                        visible: root.discoveredDevices.length === 0
                        text: root.isBusy ? "Scanning network..." : "No cameras found"
                        color: Theme.textMuted
                        font.pixelSize: 12
                    }
                }

                // Credentials row
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "ONVIF Username"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: onvifUsernameInput
                                anchors.fill: parent; anchors.margins: 6
                                text: "admin"
                                color: Theme.textPrimary; font.pixelSize: 12
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text { text: "ONVIF Password"; color: Theme.textSecondary; font.pixelSize: 11 }
                        Rectangle {
                            Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                            TextInput {
                                id: onvifPasswordInput
                                anchors.fill: parent; anchors.margins: 6
                                echoMode: TextInput.Password
                                text: ""
                                color: Theme.textPrimary; font.pixelSize: 12
                            }
                        }
                    }
                }
            }

            // Step 2: Stream Profile Review
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.currentStep === 2
                spacing: 10

                Text { text: "Camera Name"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: camNameInput
                        anchors.fill: parent; anchors.margins: 6
                        color: Theme.textPrimary; font.pixelSize: 12
                    }
                }

                Text { text: "RTSP Main Stream (1080p Recording - Zero Transcode)"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: mainUrlInput
                        anchors.fill: parent; anchors.margins: 6
                        color: Theme.textPrimary; font.family: Theme.fontMono; font.pixelSize: 11
                    }
                }

                Text { text: "RTSP Sub Stream (Live Grid & AI Analytics)"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle {
                    Layout.fillWidth: true; height: 32; color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm
                    TextInput {
                        id: subUrlInput
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
                        Text { text: "Ingress Codec"; color: Theme.textSecondary; font.pixelSize: 11 }
                        ComboBox {
                            id: codecCombo
                            Layout.fillWidth: true; height: 32
                            model: ["H.264 (VPU Hardware Copy)", "H.265 / HEVC"]
                            background: Rectangle { color: Theme.bgInput; border.color: Theme.borderSubtle; radius: Theme.radiusSm }
                            contentItem: Text { text: codecCombo.displayText; color: Theme.textPrimary; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; leftPadding: 8 }
                        }
                    }
                }
            }

            // Status message
            Text {
                Layout.fillWidth: true
                text: root.statusMessage
                color: Theme.statusWarning
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            // Footer Buttons
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
                        text: root.currentStep === 1 ? "Re-scan" : "Back"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.currentStep === 1) root.runDiscovery();
                            else root.currentStep = 1;
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 140
                    Layout.preferredHeight: 34
                    color: Theme.accentBlue
                    radius: Theme.radiusSm
                    opacity: (root.isBusy || (root.currentStep === 1 && !root.selectedDevice)) ? 0.5 : 1.0

                    Text {
                        anchors.centerIn: parent
                        text: root.currentStep === 1 ? "Probe Profiles ➔" : "Add to NVR"
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.isBusy && (root.currentStep !== 1 || root.selectedDevice !== null)
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (root.currentStep === 1) root.probeProfiles();
                            else root.commitCamera();
                        }
                    }
                }
            }
        }
    }
}
