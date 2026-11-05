import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root

    property var telemetry: ({})
    property var cameras: []
    property string statusMessage: ""

    signal navigateTo(string route)

    Component.onCompleted: {
        if (root.visible) loadData();
    }

    onVisibleChanged: {
        if (root.visible) loadData();
    }

    Timer {
        interval: 4000
        repeat: true
        running: root.visible
        onTriggered: root.loadData()
    }

    function loadData() {
        ApiClient.getSystemStatus(function(data) {
            if (!data) return;
            var totalMb = data.total_storage_bytes ? Math.round(data.total_storage_bytes / (1024 * 1024)) : 0;
            var freeMb = data.free_storage_bytes ? Math.round(data.free_storage_bytes / (1024 * 1024)) : 0;
            var ramFreeMb = data.ram_free_bytes ? Math.round(data.ram_free_bytes / (1024 * 1024)) : 0;
            var cores = (typeof data.cpu_cores === "number" && data.cpu_cores > 0) ? data.cpu_cores : 6;
            var cpuPct = (typeof data.cpu_percent === "number")
                ? Math.min(100, Math.max(0, data.cpu_percent))
                : ((typeof data.cpu_load_1m === "number") ? Math.min(100, (data.cpu_load_1m / cores) * 100) : 0.0);
            root.telemetry = {
                status: data.status || "OFFLINE",
                uptime_seconds: Number(data.uptime_seconds || 0),
                cpu_usage_pct: Number(cpuPct.toFixed(1)),
                memory_free_mb: ramFreeMb,
                storage_free_gb: (freeMb / 1024).toFixed(1),
                storage_used_gb: (Math.max(0, totalMb - freeMb) / 1024).toFixed(1),
                storage_total_gb: (totalMb / 1024).toFixed(1),
                bitrate_mbps: Number(data.bitrate_mbps || 0)
            };
        });

        ApiClient.getCameras(function(list) {
            root.cameras = list || [];
        });
    }

    ScrollView {
        id: dashScrollView
        anchors.fill: parent
        anchors.margins: 20
        clip: true

        ColumnLayout {
            width: root.width - 40
            spacing: 20

            // 1. Dashboard Header (Exact Web Parity)
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    Text {
                        text: "Enterprise Surveillance Dashboard"
                        color: Theme.textPrimary
                        font.pixelSize: 22
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                    Text {
                        text: "NXP i.MX 95 system telemetry & video channel health"
                        color: Theme.textMuted
                        font.pixelSize: 13
                        font.family: Theme.fontSans
                    }
                }

                // Header Action Buttons
                RowLayout {
                    spacing: 8

                    // Refresh Button
                    Rectangle {
                        Layout.preferredHeight: 32
                        Layout.preferredWidth: refreshDashRow.implicitWidth + 24
                        color: Theme.bgSecondary
                        border.color: Theme.borderSubtle
                        radius: 6

                        RowLayout {
                            id: refreshDashRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "refresh"
                                size: 13
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }

                            Text {
                                text: "Refresh telemetry"
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.loadData()
                        }
                    }

                    // Open Live Matrix Button
                    Rectangle {
                        Layout.preferredHeight: 32
                        Layout.preferredWidth: openLiveRow.implicitWidth + 24
                        color: Theme.accentPrimary
                        radius: 6

                        RowLayout {
                            id: openLiveRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "play"
                                size: 13
                                color: "#ffffff"
                                strokeWidth: 2
                            }

                            Text {
                                text: "Open live matrix"
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.navigateTo("live")
                        }
                    }
                }
            }

            // 2. Metrics Grid (5 KPI Stat Cards, Exact Web Parity)
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                // Card 1: Core Engine
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    height: 104
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 14

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                text: "CORE ENGINE"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontSans
                            }
                            Text {
                                text: "ONLINE"
                                color: Theme.statusOnline
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontDisplay
                            }
                            Text {
                                text: "Uptime: " + (root.telemetry.uptime_seconds ? (Math.floor(root.telemetry.uptime_seconds / 3600) + "h " + Math.floor((root.telemetry.uptime_seconds % 3600) / 60) + "m") : "Active")
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.family: Theme.fontSans
                            }
                        }

                        Rectangle {
                            width: 36
                            height: 36
                            radius: 8
                            color: Theme.bgSecondary
                            Layout.alignment: Qt.AlignTop | Qt.AlignRight

                            AppIcon {
                                anchors.centerIn: parent
                                name: "shield"
                                size: 18
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }
                        }
                    }
                }

                // Card 2: CPU Utilization
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    height: 104
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 14

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                text: "CPU UTILIZATION"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontSans
                            }
                            Text {
                                text: (root.telemetry.cpu_usage_pct !== undefined ? Number(root.telemetry.cpu_usage_pct).toFixed(1) : "0.0") + "%"
                                color: Theme.accentPrimary
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontDisplay
                            }
                            Text {
                                text: "Ceiling: under 50% target"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.family: Theme.fontSans
                            }
                        }

                        Rectangle {
                            width: 36
                            height: 36
                            radius: 8
                            color: Theme.bgSecondary
                            Layout.alignment: Qt.AlignTop | Qt.AlignRight

                            AppIcon {
                                anchors.centerIn: parent
                                name: "cpu"
                                size: 18
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }
                        }
                    }
                }

                // Card 3: Free RAM
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    height: 104
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 14

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                text: "FREE RAM"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontSans
                            }
                            Text {
                                text: (root.telemetry.memory_free_mb !== undefined ? root.telemetry.memory_free_mb : 0) + " MB"
                                color: Theme.statusWarning
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontDisplay
                            }
                            Text {
                                text: "Budget: 4 GB LPDDR5 par..."
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.family: Theme.fontSans
                            }
                        }

                        Rectangle {
                            width: 36
                            height: 36
                            radius: 8
                            color: Theme.bgSecondary
                            Layout.alignment: Qt.AlignTop | Qt.AlignRight

                            AppIcon {
                                anchors.centerIn: parent
                                name: "ram"
                                size: 18
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }
                        }
                    }
                }

                // Card 4: Storage Free
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    height: 104
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 14

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                text: "STORAGE FREE"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontSans
                            }
                            Text {
                                text: (root.telemetry.storage_free_gb !== undefined ? root.telemetry.storage_free_gb : "0.0") + " GB"
                                color: Theme.statusDanger
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontDisplay
                            }
                            Text {
                                text: "Circular retention FIFO"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.family: Theme.fontSans
                            }
                        }

                        Rectangle {
                            width: 36
                            height: 36
                            radius: 8
                            color: Theme.bgSecondary
                            Layout.alignment: Qt.AlignTop | Qt.AlignRight

                            AppIcon {
                                anchors.centerIn: parent
                                name: "storage"
                                size: 18
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }
                        }
                    }
                }

                // Card 5: Active Cameras
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    height: 104
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 14
                    clip: true

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            spacing: 3

                            Text {
                                text: "ACTIVE CAMERAS"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontSans
                            }
                            Text {
                                text: root.cameras.length + " / 8"
                                color: Theme.accentPrimary
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontDisplay
                            }
                            Text {
                                text: "Centralized ingress sessi..."
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.family: Theme.fontSans
                            }
                        }

                        Rectangle {
                            width: 36
                            height: 36
                            radius: 8
                            color: Theme.bgSecondary
                            Layout.alignment: Qt.AlignTop | Qt.AlignRight

                            AppIcon {
                                anchors.centerIn: parent
                                name: "camera"
                                size: 18
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }
                        }
                    }
                }
            }

            // 3. Storage Capacity & Retention Bar Card (Exact Web Parity)
            Rectangle {
                Layout.fillWidth: true
                height: 112
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 16

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Media storage allocation"
                            color: Theme.textPrimary
                            font.pixelSize: 15
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: (root.telemetry.storage_used_gb !== undefined ? root.telemetry.storage_used_gb : "0.0") + " GB / " + (root.telemetry.storage_total_gb !== undefined ? root.telemetry.storage_total_gb : "0.0") + " GB"
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }

                    Text {
                        text: "Direct filesystem container storage with WAL metadata index"
                        color: Theme.textMuted
                        font.pixelSize: 12
                        font.family: Theme.fontSans
                    }

                    // Progress Bar
                    Rectangle {
                        Layout.fillWidth: true
                        height: 8
                        color: Theme.bgSecondary
                        radius: 4

                        Rectangle {
                            width: (root.telemetry.storage_total_gb && Number(root.telemetry.storage_total_gb) > 0) ? Math.min(parent.width, parent.width * (Number(root.telemetry.storage_used_gb || 0) / Number(root.telemetry.storage_total_gb))) : 0
                            height: parent.height
                            radius: 4
                            color: Theme.accentPrimary
                        }
                    }

                    // Bottom telemetry line
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 24

                        RowLayout {
                            spacing: 6
                            AppIcon { name: "lightning"; size: 12; color: Theme.accentPrimary }
                            Text { text: "Ingest " + (root.telemetry.bitrate_mbps !== undefined ? Number(root.telemetry.bitrate_mbps).toFixed(1) : "0.0") + " Mbps"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                        }
                        Text { text: "Segment: 60 s atomic .mp4"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                        Text { text: "Retention: 14 days FIFO"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                    }
                }
            }

            // 4. Configured Video Channels Section (Exact Web Parity)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Configured video channels"
                        color: Theme.textPrimary
                        font.pixelSize: 18
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        width: manageBtnRow.implicitWidth + 24
                        height: 32
                        color: Theme.bgSecondary
                        border.color: Theme.borderSubtle
                        radius: 6

                        RowLayout {
                            id: manageBtnRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "list"
                                size: 13
                                color: Theme.textSecondary
                                strokeWidth: 2
                            }

                            Text {
                                text: "Manage channels"
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.navigateTo("cameras")
                        }
                    }
                }

                // If cameras configured, display camera tiles
                Flow {
                    Layout.fillWidth: true
                    spacing: 16
                    visible: root.cameras.length > 0

                    Repeater {
                        model: root.cameras
                        delegate: Rectangle {
                            width: 240
                            height: 160
                            color: Theme.bgCard
                            border.color: Theme.borderSubtle
                            radius: 12
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                Image {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    fillMode: Image.PreserveAspectCrop
                                    source: ApiClient.getSnapshotUrl(modelData.id)
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 32
                                    color: Theme.bgSecondary

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8

                                        Text {
                                            text: modelData.name || ("Channel " + modelData.id)
                                            color: Theme.textPrimary
                                            font.pixelSize: 11
                                            font.bold: true
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
